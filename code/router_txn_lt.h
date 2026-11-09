// ============================================================================
// router_txn_lt.h -- LT 对照模型：ready/valid 握手，粗粒度架构探索
//
// 与 AT 版完全相同的 spec、拓扑、路由、消息长度、链路带宽。
// 区别有两点：
//   1) 流控协议：AT 用 credit（预约缓冲、有反压）；LT 用 ready/valid，
//      下游对每次 valid 都立即 ready，没有重试、没有拥塞。
//   2) 时间模型：没有 link_free_at，因此没有排队，(t_rc + t_link +
//      msg_bytes/link_bw) 沿路径累加一遍就是全部时序。
//
// 因此 LT 适合回答「连通性对不对、带宽够不够」这类粗粒度问题：
// 它算得出一条路径有多少跳、每跳要占链路多久、需求带宽是否超过供给；
// 但算不出「延迟和拥塞怎么样」。
// ============================================================================
#ifndef ROUTER_TXN_LT_H
#define ROUTER_TXN_LT_H

#include "noc_txn_common.h"
#include <algorithm>

SC_MODULE(RouterTxnLT)
{
  typedef noc::noc_protocol_types PT;
  typedef tlm::tlm_generic_payload gp_t;

  tlm_utils::multi_passthrough_target_socket<RouterTxnLT, 64, PT> in;
  tlm_utils::simple_initiator_socket_tagged<RouterTxnLT, 64, PT>* out[noc::N_PORTS];
  tlm::tlm_analysis_port<noc::TxnRecord> stat_port;

  noc::RouterConfig cfg;
  unsigned long n_forwarded = 0;

  // ready/valid 与带宽账统计（纯观测，不影响时序）
  unsigned long n_handshake = 0;      // 成功握手的跳数
  unsigned long n_not_ready = 0;      // ready=0 导致的重试（LT 恒 0）
  unsigned long long bytes_offered = 0; // 本 router 转发的总字节数

  RouterTxnLT(sc_core::sc_module_name nm, noc::RouterConfig c)
    : sc_core::sc_module(nm), in("in"), cfg(c)
  {
    for (int p = 0; p < noc::N_PORTS; ++p) {
      out[p] = new tlm_utils::simple_initiator_socket_tagged<RouterTxnLT, 64, PT>(
          sc_core::sc_gen_unique_name("out"));
      out[p]->register_invalidate_direct_mem_ptr(this, &RouterTxnLT::invalidate_dmi, p);
    }
    in.register_b_transport(this, &RouterTxnLT::b_fw);
    in.register_transport_dbg(this, &RouterTxnLT::dbg_fw);
    in.register_get_direct_mem_ptr(this, &RouterTxnLT::dmi_fw);
  }

  void b_fw(int /*bind*/, gp_t& trans, sc_core::sc_time& delay)
  {
    noc::txn_ext* ex = trans.get_extension<noc::txn_ext>();
    noc::rv_ext*  rv = trans.get_extension<noc::rv_ext>();

    // ---- ready/valid：valid 才是一次有效传输；LT 没有队列，永远 ready ----
    if (rv) {
      if (!rv->valid) {
        trans.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;                                   // 没有数据要约，握手不成立
      }
      if (!rv->ready) ++n_not_ready;              // LT 下不会发生
      rv->ready = true;                           // 本跳接收
      ++n_handshake;
    }

    int dx = cfg.x, dy = cfg.y;
    if (ex) noc::id2xy(ex->dst, cfg.mesh_x, dx, dy);
    const int op = noc::route_port(cfg.topology, cfg.routing,
                                   cfg.x, cfg.y, dx, dy,
                                   cfg.mesh_x, cfg.mesh_y, ex ? ex->rt_choice : 0);

    // 负载无关的固定服务时间 —— LT 的全部时序
    const sc_core::sc_time service =
        cfg.t_rc() + cfg.t_service(ex ? ex->msg_bytes : cfg.msg_bytes);
    delay += service;
    if (rv) rv->handshake_at = sc_core::sc_time_stamp() + delay;

    if (ex) {
      ++ex->hops;
      ex->in_port = noc::opposite(op);
      bytes_offered += ex->msg_bytes;
      noc::TxnRecord r;
      r.kind = noc::TxnRecord::R_HOP;
      r.msg_id = ex->msg_id; r.hops = ex->hops;
      r.inject_time = ex->inject_time;
      r.now = sc_core::sc_time_stamp() + delay;
      stat_port.write(r);
    }
    ++n_forwarded;
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
    (*out[op])->b_transport(trans, delay);
  }

  // 一跳要占用链路多久：LT 算「带宽够不够」的唯一依据
  sc_core::sc_time hop_service(int bytes) const {
    return cfg.t_rc() + cfg.t_service(bytes);
  }

  unsigned int dbg_fw(int /*bind*/, gp_t& trans)
  {
    unsigned char* p = trans.get_data_ptr();
    if (!p || trans.get_data_length() < 4) return 0;
    const unsigned long v = n_handshake;
    for (int i = 0; i < 4; ++i) p[i] = (unsigned char)((v >> (8 * i)) & 0xff);
    return 4;
  }

  bool dmi_fw(int /*bind*/, gp_t& trans, tlm::tlm_dmi&)
  {
    trans.set_dmi_allowed(false);
    return false;
  }
  void invalidate_dmi(int, sc_dt::uint64, sc_dt::uint64) {}
};

#endif // ROUTER_TXN_LT_H
