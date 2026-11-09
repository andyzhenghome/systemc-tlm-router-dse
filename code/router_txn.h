// ============================================================================
// router_txn.h -- 架构探索用 AT router：transaction 级 + credit 流控
//
// 一个事务 = 一条完整消息。不做逐片切分、不做 RC/VA/SA/ST 流水状态机。
// 关键建模决策：把「竞争」从流水线部件上移到输出链路，
//
//     start   = max(arrival + t_rc, link_free_at[op])
//     service = t_link + msg_bytes / link_bw
//     link_free_at[op] = start + service
//     delay  += (start + service - arrival)          <-- 时间注解，不是 wait()
//
// 于是拥塞 = start - arrival - t_rc（排队等待），饱和吞吐 =
// link_bw / avg_msg_bytes。下游在 now+delay 看到该事务，链路利用率、尾延迟、
// 热点都能统计出来，而不需要逐周期调度。
//
// 流控协议是 credit-based：
//   NI 持有 ni_window 个 credit，注入一条消息消耗一个；
//   本跳链路服务完成（LINK_DONE）时释放输入槽，用 BEGIN_RESP 把一个
//   credit 沿四相反向路径还给上游，上游 END_RESP 闭合这一跳。
//   中间 router 的缓冲无硬上限（排队网络假设），credit 只做守恒统计。
//
// 四相握手仍然完整：
//   BEGIN_REQ -> (fw, 带 delay 注解) -> 下一跳
//   END_REQ   -> PEQ 在 arrival 时刻经 bw 回给上游
//   BEGIN_RESP-> 链路服务完成，归还一个 credit，经 bw 回给上游
//   END_RESP  -> 上游确认，本跳事务结束
// 扩展相位 CREDIT 用于反向路径上「归还事件」的时间注解。
//
// 这就是「延迟和拥塞怎么样」的细粒度探索：能给出延迟-负载曲线、
// 饱和点、排队等待分布与最忙链路。
// ============================================================================
#ifndef ROUTER_TXN_H
#define ROUTER_TXN_H

#include "noc_txn_common.h"
#include <map>
#include <algorithm>

SC_MODULE(RouterTxn)
{
  typedef noc::noc_protocol_types PT;
  typedef tlm::tlm_generic_payload gp_t;
  typedef tlm::tlm_phase           ph_t;

  tlm_utils::multi_passthrough_target_socket<RouterTxn, 64, PT> in;
  tlm_utils::simple_initiator_socket_tagged<RouterTxn, 64, PT>* out[noc::N_PORTS];

  tlm::tlm_analysis_port<noc::TxnRecord> stat_port;

  noc::RouterConfig cfg;

  // 每个输出端口就是一个 server：link_free_at 是它的「忙到什么时候」
  std::vector<sc_core::sc_time> link_free_at;
  std::vector<sc_core::sc_time> busy_time;        // 累计服务时间 -> 链路利用率
  std::vector<sc_core::sc_time> busy_begin;       // 该端口第一次开始服务的时刻
  std::vector<bool>             first_service;
  std::vector<unsigned long>    n_served;
  std::vector<int>              occupancy;        // 输入端口当前占用的 message slot
  std::vector<int>              max_occupancy;

  // credit 守恒统计：issued = 收到并接受的请求数，
  // returned = 因链路服务完成而归还给上游的 credit 数。
  unsigned long n_credit_issued = 0, n_credit_returned = 0;

  std::map<gp_t*, noc::HopCtx> rx_ctx, tx_ctx;
  tlm_utils::peq_with_cb_and_phase<RouterTxn, PT> peq;

  unsigned long n_forwarded = 0, n_queued = 0;
  double sum_wait_ns = 0.0, max_wait_ns = 0.0;

  SC_HAS_PROCESS(RouterTxn);

  RouterTxn(sc_core::sc_module_name nm, noc::RouterConfig c)
    : sc_core::sc_module(nm)
    , in("in")
    , cfg(c)
    , peq("peq", this, &RouterTxn::peq_cb)
  {
    for (int p = 0; p < noc::N_PORTS; ++p) {
      out[p] = new tlm_utils::simple_initiator_socket_tagged<RouterTxn, 64, PT>(
          sc_core::sc_gen_unique_name("out"));
      out[p]->register_nb_transport_bw(this, &RouterTxn::nb_bw, p);
      out[p]->register_invalidate_direct_mem_ptr(this, &RouterTxn::invalidate_dmi, p);
    }
    in.register_nb_transport_fw(this, &RouterTxn::nb_fw);
    in.register_b_transport    (this, &RouterTxn::b_fw);
    in.register_transport_dbg  (this, &RouterTxn::dbg_fw);
    in.register_get_direct_mem_ptr(this, &RouterTxn::dmi_fw);

    link_free_at.assign (noc::N_PORTS, sc_core::SC_ZERO_TIME);
    busy_time.assign    (noc::N_PORTS, sc_core::SC_ZERO_TIME);
    busy_begin.assign   (noc::N_PORTS, sc_core::SC_ZERO_TIME);
    first_service.assign(noc::N_PORTS, true);
    n_served.assign     (noc::N_PORTS, 0);
    occupancy.assign   (noc::N_PORTS, 0);
    max_occupancy.assign(noc::N_PORTS, 0);
  }

  // 链路利用率 0..1：分母取「该端口第一次开始服务到最后一次服务结束」，
  // 而不是全局观测窗口，否则排队延伸到窗口之外时会算出 >1 的假值。
  double link_utilization(int p) const {
    if (first_service[p]) return 0.0;
    const double s = (link_free_at[p] - busy_begin[p]).to_seconds();
    return (s > 0.0) ? busy_time[p].to_seconds() / s : 0.0;
  }

  // -------------------------------------------------------------------------
  // 前向路径（本 router 作为 target）
  // -------------------------------------------------------------------------
  tlm::tlm_sync_enum nb_fw(int bind, gp_t& trans, ph_t& phase, sc_core::sc_time& delay)
  {
    if (phase == tlm::BEGIN_REQ) {
      noc::txn_ext* ex = trans.get_extension<noc::txn_ext>();
      if (!ex) {
        trans.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return tlm::TLM_COMPLETED;
      }
      const int ip = ex->in_port;
      if (ip < 0 || ip >= cfg.n_ports) {
        trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        return tlm::TLM_COMPLETED;
      }

      const sc_core::sc_time now = sc_core::sc_time_stamp();
      const sc_core::sc_time arrival = now + delay;

      int dx, dy;
      noc::id2xy(ex->dst, cfg.mesh_x, dx, dy);
      const int op = noc::route_port(cfg.topology, cfg.routing,
                                     cfg.x, cfg.y, dx, dy,
                                     cfg.mesh_x, cfg.mesh_y, ex->rt_choice);

      // ---- 链路服务模型：架构探索看到的所有拥塞都来自这里 ----
      const sc_core::sc_time earliest = arrival + cfg.t_rc();
      const sc_core::sc_time start   = std::max(earliest, link_free_at[op]);
      const sc_core::sc_time service = cfg.t_service(ex->msg_bytes);
      const sc_core::sc_time done_at = start + service;
      link_free_at[op] = done_at;
      busy_time[op]   += service;
      if (first_service[op]) { busy_begin[op] = start; first_service[op] = false; }
      ++n_served[op];

      const double qwait =
          (start - earliest).to_seconds() * 1e9;   // 纯排队，不含 t_rc
      if (qwait > 0.0) {
        ++n_queued;
        sum_wait_ns += qwait;
        if (qwait > max_wait_ns) max_wait_ns = qwait;
      }

      ++occupancy[ip];                        // 占用上游的一个 credit
      ++n_credit_issued;
      if (occupancy[ip] > max_occupancy[ip]) max_occupancy[ip] = occupancy[ip];

      trans.acquire();                            // 本 router 的输入缓冲持有一份
      noc::HopCtx h;
      h.port = ip; h.bind = bind; h.out_port = op;
      h.arrival = arrival; h.done_at = done_at; h.wait_ns = qwait;
      rx_ctx[&trans] = h;
      ex->out_port = op;

      // 标准四相的 END_REQ：在「请求被接受的时刻」经 bw 回给上游
      ph_t p_end_req(tlm::END_REQ);
      peq.notify(trans, p_end_req, delay);

      // 扩展相位 LINK_DONE：链路服务完成的时刻（只管反向路径，
      // 前向路径已经带着 delay 注解同步推进到下一跳了）
      peq.notify(trans, noc::LINK_DONE, done_at - now);

      // 统计旁路：零时序影响
      noc::TxnRecord r;
      r.kind = noc::TxnRecord::R_QUEUE;
      r.msg_id = ex->msg_id; r.hops = ex->hops;
      r.inject_time = ex->inject_time;
      r.now = done_at; r.wait_ns = qwait;
      stat_port.write(r);

      // ---- 立刻转发，把整跳耗时累加进 delay 注解 ----
      // 注意：不是 wait。下游在 now+delay 语义上处理这个事务。
      delay = done_at - now;
      ++ex->hops;
      ex->in_port = noc::opposite(op);
      ++n_forwarded;

      trans.acquire();                            // 本 router 作为 initiator 的引用
      tx_ctx[&trans] = noc::HopCtx{op, op, op, arrival, done_at, qwait};
      ph_t p_begin_req(tlm::BEGIN_REQ);
      tlm::tlm_sync_enum s = (*out[op])->nb_transport_fw(trans, p_begin_req, delay);
      if (s == tlm::TLM_COMPLETED) {              // sink 早完成，没有 bw 相位
        tx_ctx.erase(&trans);
        trans.release();
      }
      return tlm::TLM_ACCEPTED;
    }

    if (phase == tlm::END_RESP) {
      rx_ctx.erase(&trans);
      trans.release();                           // 上游已确认，四相闭合
      return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
  }

  // -------------------------------------------------------------------------
  // 反向路径（本 router 作为 initiator，下游 target 回调）
  // -------------------------------------------------------------------------
  tlm::tlm_sync_enum nb_bw(int id, gp_t& trans, ph_t& phase, sc_core::sc_time& delay)
  {
    (void)id;
    if (phase == tlm::END_REQ) return tlm::TLM_ACCEPTED;

    if (phase == tlm::BEGIN_RESP) {
      // 下游归还了一个 credit：用扩展相位保留它携带的时间注解，
      // 再以标准四相 END_RESP 闭合这一跳。
      trans.acquire();
      peq.notify(trans, noc::CREDIT, delay);
      ph_t p_end_resp(tlm::END_RESP);
      sc_core::sc_time z = sc_core::SC_ZERO_TIME;
      (*out[id])->nb_transport_fw(trans, p_end_resp, z);
      trans.release();
      return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
  }

  // -------------------------------------------------------------------------
  // PEQ 回调：只负责反向路径的时间事件，不驱动任何流水线
  // -------------------------------------------------------------------------
  void peq_cb(gp_t& trans, const ph_t& phase)
  {
    if (phase == tlm::END_REQ || phase == tlm::BEGIN_RESP ||
        phase == noc::LINK_DONE) {
      auto it = rx_ctx.find(&trans);
      if (it == rx_ctx.end()) return;
      // 关键：nb_transport_bw 会同步触发上游的 END_RESP，进而重入本 router 的
      // nb_fw(END_RESP) 并 erase 掉 rx_ctx 这一项。必须先取值再调用，
      // 否则 it 立刻变成悬空迭代器（真实踩过的坑，见文章「踩坑清单」）。
      const int bind = it->second.bind;
      const int port = it->second.port;

      if (phase == noc::LINK_DONE) {
        --occupancy[port];                       // 输入 message slot 释放
        ++n_credit_returned;                    // 归还一个 credit
        ph_t p(tlm::BEGIN_RESP);                // BEGIN_RESP ≡ 一个 credit
        sc_core::sc_time d = cfg.t_credit();
        in[bind]->nb_transport_bw(trans, p, d); // 把 credit 用四相送回上游
      } else {
        ph_t p(phase);
        sc_core::sc_time z = sc_core::SC_ZERO_TIME;
        in[bind]->nb_transport_bw(trans, p, z);
      }
      return;
    }
    if (phase == noc::CREDIT) {
      tx_ctx.erase(&trans);
      trans.release();                           // 释放 initiator 侧引用
    }
  }

  // -------------------------------------------------------------------------
  // 混合调用兜底 + debug / DMI
  // -------------------------------------------------------------------------
  void b_fw(int /*bind*/, gp_t& trans, sc_core::sc_time& delay)
  {
    noc::txn_ext* ex = trans.get_extension<noc::txn_ext>();
    int dx = cfg.x, dy = cfg.y;
    if (ex) noc::id2xy(ex->dst, cfg.mesh_x, dx, dy);
    const int op = noc::route_port(cfg.topology, cfg.routing,
                                   cfg.x, cfg.y, dx, dy,
                                   cfg.mesh_x, cfg.mesh_y, ex ? ex->rt_choice : 0);
    delay += cfg.t_rc() + cfg.t_service(ex ? ex->msg_bytes : cfg.msg_bytes);
    if (ex) { ++ex->hops; ex->in_port = noc::opposite(op); }
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
    (*out[op])->b_transport(trans, delay);
  }

  unsigned int dbg_fw(int /*bind*/, gp_t& trans)
  {
    unsigned char* p = trans.get_data_ptr();
    if (!p) return 0;
    const unsigned int n = std::min<unsigned int>(trans.get_data_length(),
                                                  noc::N_PORTS);
    for (unsigned int i = 0; i < n; ++i)
      p[i] = static_cast<unsigned char>(occupancy[i] & 0xff);
    return n;                                    // 快照，不推进时间、不改状态
  }

  bool dmi_fw(int /*bind*/, gp_t& trans, tlm::tlm_dmi& /*dmi*/)
  {
    trans.set_dmi_allowed(false);               // 有状态互连，正确做法是拒绝
    return false;
  }

  void invalidate_dmi(int /*port*/, sc_dt::uint64, sc_dt::uint64) {}
};

#endif // ROUTER_TXN_H
