// ============================================================================
// router_at.h -- Approximately-Timed (AT) 参数化 VC router
//
// spec: Mullins/West/Moore, ASPDAC 2006, Fig.1 "A Virtual-Channel Router"
//       + BookSim2 IQRouter 的 RC -> VA -> SA -> ST 四级流水
//
// 建模要点：
//   * 一个 flit = 一个 tlm_generic_payload（端到端复用同一个对象）
//   * 标准四相跑在 socket 上；扩展相位跑在 PEQ 里驱动内部流水线
//   * BEGIN_RESP 的语义被定义为「下游消费了 flit，归还一个 credit」
//   * 仲裁用时钟驱动的 evaluate/update 两阶段，避免同周期信息泄漏
// ============================================================================
#ifndef ROUTER_AT_H
#define ROUTER_AT_H

#include "noc_common.h"
#include <map>
#include <algorithm>

SC_MODULE(RouterAT)
{
  typedef noc::noc_protocol_types PT;
  typedef tlm::tlm_generic_payload gp_t;
  typedef tlm::tlm_phase           ph_t;

  // 一个 multi-passthrough target socket 接所有上游链路；
  // 五个 tagged initiator socket 分别对应 N/E/S/W/L，tag == 端口号。
  tlm_utils::multi_passthrough_target_socket<RouterAT, 32, PT> in;
  tlm_utils::simple_initiator_socket_tagged<RouterAT, 32, PT>* out[noc::N_PORTS];

  sc_core::sc_in<bool> clk;
  tlm::tlm_analysis_port<noc::FlitRecord> stat_port;

  noc::RouterConfig cfg;
  noc::flit_mm*     mm;

  struct VC {
    std::deque<gp_t*> fifo;
    enum State { IDLE, RC, VA, ACTIVE } state = IDLE;
    int  out_port = -1, out_vc = -1;
    bool sa_granted = false;      // 已被 SA 选中、ST 尚未完成
  };

  std::vector<std::vector<VC>>        vc;         // [in_port][vc]
  std::vector<std::vector<int>>       credit;     // [out_port][out_vc] 下游空槽数
  std::vector<std::vector<bool>>      ovc_busy;   // [out_port][out_vc] 输出 VC 被包占用
  std::vector<int>                    rr_in, rr_out, rr_va;

  // 本 router 作为 target / initiator 时的逐事务链路上下文
  std::map<gp_t*, noc::LinkCtx> rx_ctx, tx_ctx;

  tlm_utils::peq_with_cb_and_phase<RouterAT, PT> peq;

  unsigned long n_forwarded = 0, n_sa_conflict = 0, n_va_stall = 0, n_credit_stall = 0;

  SC_HAS_PROCESS(RouterAT);

  RouterAT(sc_core::sc_module_name nm, noc::RouterConfig c, noc::flit_mm* m)
    : sc_core::sc_module(nm)
    , in("in")
    , cfg(c)
    , mm(m)
    , peq("peq", this, &RouterAT::peq_cb)
  {
    for (int p = 0; p < noc::N_PORTS; ++p) {
      out[p] = new tlm_utils::simple_initiator_socket_tagged<RouterAT, 32, PT>(
          sc_core::sc_gen_unique_name("out"));
      out[p]->register_nb_transport_bw(this, &RouterAT::nb_bw, p);
      out[p]->register_invalidate_direct_mem_ptr(this, &RouterAT::invalidate_dmi, p);
    }

    in.register_nb_transport_fw(this, &RouterAT::nb_fw);
    in.register_b_transport    (this, &RouterAT::b_fw);   // LRM 要求能处理混合调用
    in.register_transport_dbg  (this, &RouterAT::dbg_fw);
    in.register_get_direct_mem_ptr(this, &RouterAT::dmi_fw);

    SC_METHOD(arbitrate);
    sensitive << clk.pos();
    dont_initialize();

    vc.assign(cfg.n_ports, std::vector<VC>(cfg.n_vcs));
    credit.assign  (cfg.n_ports, std::vector<int>(cfg.n_vcs, cfg.buf_depth));
    ovc_busy.assign(cfg.n_ports, std::vector<bool>(cfg.n_vcs, false));
    rr_in.assign (cfg.n_ports, 0);
    rr_out.assign(cfg.n_ports, 0);
    rr_va.assign (cfg.n_ports, 0);
  }

  // -------------------------------------------------------------------------
  // 前向路径（本 router 作为 target）
  // -------------------------------------------------------------------------
  tlm::tlm_sync_enum nb_fw(int bind, gp_t& trans, ph_t& phase, sc_core::sc_time& delay)
  {
    if (phase == tlm::BEGIN_REQ) {
      noc::flit_ext* ex = trans.get_extension<noc::flit_ext>();
      if (!ex) {
        trans.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return tlm::TLM_COMPLETED;
      }
      const int ip = ex->in_port, iv = ex->in_vc;
      if (ip < 0 || ip >= cfg.n_ports || iv < 0 || iv >= cfg.n_vcs) {
        trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        return tlm::TLM_COMPLETED;
      }
      VC& V = vc[ip][iv];
      if (static_cast<int>(V.fifo.size()) >= cfg.buf_depth) {
        // credit 机制正常情况下不会走到这里；走到这里说明上游模型违约
        SC_REPORT_WARNING("RouterAT", "input VC overflow: credit protocol violated");
        trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
        return tlm::TLM_COMPLETED;
      }

      trans.acquire();                       // 本 router 持有一份引用
      V.fifo.push_back(&trans);
      rx_ctx[&trans] = noc::LinkCtx{ip, iv, bind};

      if (noc::is_head(ex->kind) && V.state == VC::IDLE) {
        V.state = VC::RC;
        peq.notify(trans, noc::RC_DONE, delay + cfg.t_rc());
      }
      // body / tail 不需要 RC、VA，由 arbitration clock 直接参与 SA

      ph_t p_end_req(tlm::END_REQ);
      peq.notify(trans, p_end_req, delay);   // END_REQ 走反向路径，异步回调
      return tlm::TLM_ACCEPTED;              // 后续相位由 PEQ + bw 调用驱动
    }

    if (phase == tlm::END_RESP) {
      // 上游确认收到 BEGIN_RESP，本 target 的事务结束
      rx_ctx.erase(&trans);
      trans.release();
      return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
  }

  // -------------------------------------------------------------------------
  // 反向路径（本 router 作为 initiator，下游 target 回调）
  //   id = tagged socket 的 tag，也就是输出端口
  // -------------------------------------------------------------------------
  tlm::tlm_sync_enum nb_bw(int id, gp_t& trans, ph_t& phase, sc_core::sc_time& delay)
  {
    if (phase == tlm::END_REQ)
      return tlm::TLM_ACCEPTED;   // 下游已接受，无需动作

    if (phase == tlm::BEGIN_RESP) {
      // BEGIN_RESP == credit：下游把这个 flit 从输入缓冲里消费掉了
      trans.acquire();                        // 保活到 credit 事件真正触发
      peq.notify(trans, noc::CREDIT, delay); // 尊重 bw 路径上的时间注解

      ph_t p_end_resp(tlm::END_RESP);
      sc_core::sc_time z = sc_core::SC_ZERO_TIME;
      (*out[id])->nb_transport_fw(trans, p_end_resp, z);
      trans.release();                        // 释放本 initiator 的引用
      return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
  }

  // -------------------------------------------------------------------------
  // PEQ 回调：内部流水线的状态机
  // -------------------------------------------------------------------------
  void peq_cb(gp_t& trans, const ph_t& phase)
  {
    noc::flit_ext* ex = trans.get_extension<noc::flit_ext>();
    if (!ex) return;

    // --- END_REQ：把「已接受」通知回上游（标准四相之一） ---
    if (phase == tlm::END_REQ) {
      auto it = rx_ctx.find(&trans);
      if (it == rx_ctx.end()) return;
      ph_t p(tlm::END_REQ);
      sc_core::sc_time z = sc_core::SC_ZERO_TIME;
      in[it->second.bind]->nb_transport_bw(trans, p, z);
      return;
    }

    // --- RC_DONE：head flit 完成路由计算，进入 VA ---
    if (phase == noc::RC_DONE) {
      auto it = rx_ctx.find(&trans);
      if (it == rx_ctx.end()) return;
      VC& V = vc[it->second.port][it->second.vc];
      V.out_port = noc::route_to_id(cfg.x, cfg.y, ex->dst, cfg.mesh_x);
      V.state = VC::VA;
      peq.notify(trans, noc::VA_REQ, cfg.t_va());
      return;
    }

    // --- VA_REQ：为整个 packet 申请一个输出 VC（atomic VC allocation） ---
    if (phase == noc::VA_REQ) {
      auto it = rx_ctx.find(&trans);
      if (it == rx_ctx.end()) return;
      VC& V = vc[it->second.port][it->second.vc];
      const int op = V.out_port;
      if (op == noc::P_L) {                 // 本地 eject，不需要下游 VC
        V.out_vc = 0;
        V.state = VC::ACTIVE;
        peq.notify(trans, noc::VA_GRANT, sc_core::SC_ZERO_TIME);
        return;
      }
      for (int k = 0; k < cfg.n_vcs; ++k) {
        const int v = (rr_va[op] + k) % cfg.n_vcs;
        if (!ovc_busy[op][v]) {
          ovc_busy[op][v] = true;
          rr_va[op] = (v + 1) % cfg.n_vcs;
          V.out_vc = v;
          V.state = VC::ACTIVE;
          peq.notify(trans, noc::VA_GRANT, sc_core::SC_ZERO_TIME);
          return;
        }
      }
      ++n_va_stall;                          // 没有空闲输出 VC，下一周期重试
      peq.notify(trans, noc::VA_REQ, cfg.clk);
      return;
    }

    if (phase == noc::VA_GRANT) return;     // 状态已置 ACTIVE，等仲裁时钟

    // --- CREDIT：反向路径时间事件到期，真正归还 credit ---
    if (phase == noc::CREDIT) {
      auto it = tx_ctx.find(&trans);
      if (it != tx_ctx.end()) {
        const int op = it->second.port, ov = it->second.vc;
        if (op != noc::P_L) ++credit[op][ov];
        tx_ctx.erase(it);
      }
      trans.release();                       // 释放 nb_bw 里临时加的引用
      return;
    }

    // --- BEGIN_RESP（本 router 作为 target）：把 credit 送回上游 ---
    if (phase == tlm::BEGIN_RESP) {
      auto it = rx_ctx.find(&trans);
      if (it == rx_ctx.end()) return;
      ph_t p(tlm::BEGIN_RESP);
      sc_core::sc_time z = sc_core::SC_ZERO_TIME;
      in[it->second.bind]->nb_transport_bw(trans, p, z);
      return;
    }

    // --- ST_DONE：crossbar 穿越完成，flit 离开本 router ---
    if (phase == noc::ST_DONE) {
      auto it = rx_ctx.find(&trans);
      if (it == rx_ctx.end()) return;
      const int ip = it->second.port, iv = it->second.vc;
      VC& V = vc[ip][iv];
      if (V.fifo.empty() || V.fifo.front() != &trans) return;  // 过期事件
      V.fifo.pop_front();
      V.sa_granted = false;

      const int op = V.out_port, ov = V.out_vc;
      const bool local = (op == noc::P_L);
      if (!local) --credit[op][ov];         // 发送时扣一个 credit

      if (noc::is_tail(ex->kind)) {         // tail 释放输出 VC 与输入 VC 状态
        if (!local) ovc_busy[op][ov] = false;
        V.state = VC::IDLE;
        V.out_port = V.out_vc = -1;
        // 同一个 VC 里可能已经排着下一个 packet 的 head flit
        // （credit 归还后 NI 就可以继续注入）。此刻才轮到它做 RC。
        if (!V.fifo.empty()) {
          noc::flit_ext* nx = V.fifo.front()->get_extension<noc::flit_ext>();
          if (nx && noc::is_head(nx->kind)) {
            V.state = VC::RC;
            peq.notify(*V.fifo.front(), noc::RC_DONE, cfg.t_rc());
          }
        }
      }

      // 逐跳字段重写：给下一跳用
      ex->in_port = noc::opposite(op);
      ex->in_vc   = ov;
      ++ex->hops;

      // 统计旁路输出（analysis port，不占事务通路、不影响时序）
      noc::FlitRecord r;
      r.kind = noc::FlitRecord::R_HOP;
      r.pkt_id = ex->pkt_id; r.hops = ex->hops;
      r.inject_time = ex->inject_time; r.now = sc_core::sc_time_stamp();
      stat_port.write(r);
      ++n_forwarded;

      // 先把「buffer slot 空出来」这件事告诉上游（= credit）
      ph_t p_begin_resp(tlm::BEGIN_RESP);
      peq.notify(trans, p_begin_resp, cfg.t_credit());

      // 再向下一跳发 BEGIN_REQ；delay 是时间注解，不是 wait
      trans.acquire();
      tx_ctx[&trans] = noc::LinkCtx{op, ov, op};
      ph_t p_begin_req(tlm::BEGIN_REQ);
      sc_core::sc_time d = cfg.t_link();
      tlm::tlm_sync_enum s = (*out[op])->nb_transport_fw(trans, p_begin_req, d);

      if (s == tlm::TLM_COMPLETED) {
        // 早完成（本地 sink / NullSink）：不再有 END_REQ、BEGIN_RESP、END_RESP
        tx_ctx.erase(&trans);
        trans.release();
      }
    }
  }

  // -------------------------------------------------------------------------
  // 仲裁：每个时钟沿做一次 separable allocation (SA-i + SA-o)
  // 故意拆成 evaluate / update 两段：evaluate 只读状态并生成 grant 列表，
  // update 才改状态。BookSim2 ISPASS'13 指出串行更新会让 SA 提前看到
  // VA 的结果，从而抹掉 misspeculation —— AT 模型必须避开这个坑。
  // -------------------------------------------------------------------------
  void arbitrate()
  {
    std::vector<int> in_winner (cfg.n_ports, -1);
    std::vector<int> out_winner(cfg.n_ports, -1);

    // ---- evaluate：只读 ----
    for (int ip = 0; ip < cfg.n_ports; ++ip) {
      int requests = 0;
      for (int k = 0; k < cfg.n_vcs; ++k) {
        const int v = (rr_in[ip] + k) % cfg.n_vcs;
        const VC& V = vc[ip][v];
        if (V.state != VC::ACTIVE || V.sa_granted || V.fifo.empty()) continue;
        const int op = V.out_port;
        if (op < 0) continue;
        if (op != noc::P_L && credit[op][V.out_vc] <= 0) { ++n_credit_stall; continue; }
        ++requests;
      }
      if (requests > 1) n_sa_conflict += (requests - 1);
      for (int k = 0; k < cfg.n_vcs; ++k) {
        const int v = (rr_in[ip] + k) % cfg.n_vcs;
        const VC& V = vc[ip][v];
        if (V.state != VC::ACTIVE || V.sa_granted || V.fifo.empty()) continue;
        const int op = V.out_port;
        if (op < 0) continue;
        if (op != noc::P_L && credit[op][V.out_vc] <= 0) continue;
        in_winner[ip] = v;
        break;
      }
    }
    for (int op = 0; op < cfg.n_ports; ++op) {
      for (int k = 0; k < cfg.n_ports; ++k) {
        const int ip = (rr_out[op] + k) % cfg.n_ports;
        const int v = in_winner[ip];
        if (v < 0) continue;
        if (vc[ip][v].out_port != op) continue;
        out_winner[op] = ip;
        break;
      }
    }

    // ---- update：统一改状态 ----
    for (int op = 0; op < cfg.n_ports; ++op) {
      const int ip = out_winner[op];
      if (ip < 0) continue;
      const int v = in_winner[ip];
      VC& V = vc[ip][v];
      V.sa_granted = true;
      rr_in[ip]  = (v + 1) % cfg.n_vcs;
      rr_out[op] = (ip + 1) % cfg.n_ports;
      peq.notify(*V.fifo.front(), noc::ST_DONE, cfg.t_sa() + cfg.t_st());
    }
  }

  // -------------------------------------------------------------------------
  // 其余 TLM 接口
  // -------------------------------------------------------------------------
  void b_fw(int /*bind*/, gp_t& trans, sc_core::sc_time& delay)
  {
    // 混合 b_transport / nb_transport 的兜底路径（LRM 要求 target 能处理）
    delay += cfg.t_router() + cfg.t_link();
    noc::flit_ext* ex = trans.get_extension<noc::flit_ext>();
    const int op = ex ? noc::route_to_id(cfg.x, cfg.y, ex->dst, cfg.mesh_x) : noc::P_L;
    if (ex) ++ex->hops;
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
    (*out[op])->b_transport(trans, delay);
  }

  unsigned int dbg_fw(int /*bind*/, gp_t& trans)
  {
    // debug transport：无副作用、不推进时间、不 pop FIFO，
    // 只是把各 VC 的当前占用率快照拷给调试器
    unsigned char* p = trans.get_data_ptr();
    if (!p) return 0;
    const unsigned int n = std::min<unsigned int>(
        trans.get_data_length(),
        static_cast<unsigned int>(cfg.n_ports * cfg.n_vcs));
    for (unsigned int i = 0; i < n; ++i)
      p[i] = static_cast<unsigned char>(vc[i / cfg.n_vcs][i % cfg.n_vcs].fifo.size());
    return n;
  }

  bool dmi_fw(int /*bind*/, gp_t& trans, tlm::tlm_dmi& /*dmi*/)
  {
    // router 是有状态互连而不是 memory，给不出稳定的直访指针。
    // TLM 的正确做法就是拒绝 DMI（返回 false），而不是硬造一个。
    trans.set_dmi_allowed(false);
    return false;
  }

  void invalidate_dmi(int /*port*/, sc_dt::uint64 /*start*/, sc_dt::uint64 /*end*/)
  {
    // 下游撤销 DMI 时的 bw 回调（本模型不用 DMI，留空即可）
  }
};

#endif // ROUTER_AT_H
