// ============================================================================
// router_lt.h -- Loosely-Timed (LT) 版 router，用于和 AT 对照
//
// 同样的 spec、同样的 XY 路由、同样的端口，但：
//   * 只有 b_transport，一次函数调用走完一跳（2 个时间点）
//   * 不建模 VC / FIFO / arbiter / crossbar 竞争 / credit 反压
//   * 时间靠 sc_time& delay 一路累加，配合 initiator 的 quantum keeper
//     做 temporal decoupling，因此仿真极快
// ============================================================================
#ifndef ROUTER_LT_H
#define ROUTER_LT_H

#include "noc_common.h"
#include <algorithm>

SC_MODULE(RouterLT)
{
  typedef noc::noc_protocol_types PT;
  typedef tlm::tlm_generic_payload gp_t;

  tlm_utils::multi_passthrough_target_socket<RouterLT, 32, PT> in;
  tlm_utils::simple_initiator_socket_tagged<RouterLT, 32, PT>* out[noc::N_PORTS];
  tlm::tlm_analysis_port<noc::FlitRecord> stat_port;

  noc::RouterConfig cfg;
  int lt_extra_cycles = 0;      // 统计/回归得到的「平均排队延迟」占位
  unsigned long n_forwarded = 0;

  RouterLT(sc_core::sc_module_name nm, noc::RouterConfig c)
    : sc_core::sc_module(nm)
    , in("in")
    , cfg(c)
  {
    for (int p = 0; p < noc::N_PORTS; ++p) {
      out[p] = new tlm_utils::simple_initiator_socket_tagged<RouterLT, 32, PT>(
          sc_core::sc_gen_unique_name("out"));
      out[p]->register_invalidate_direct_mem_ptr(this, &RouterLT::invalidate_dmi, p);
    }
    in.register_b_transport(this, &RouterLT::b_fw);
    in.register_transport_dbg(this, &RouterLT::dbg_fw);
    in.register_get_direct_mem_ptr(this, &RouterLT::dmi_fw);
  }

  // -------------------------------------------------------------------------
  // LT 的全部时序：把这一跳的延迟累加进 delay，然后同步调用下一跳
  // -------------------------------------------------------------------------
  void b_fw(int /*bind*/, gp_t& trans, sc_core::sc_time& delay)
  {
    noc::flit_ext* ex = trans.get_extension<noc::flit_ext>();
    const int op = ex ? noc::route_to_id(cfg.x, cfg.y, ex->dst, cfg.mesh_x) : noc::P_L;

    // 四级流水 + 链路 +（可选的）平均排队惩罚。
    // 注意：这里没有「谁排在谁前面」的概念，也没有 buffer 满的概念。
    delay += cfg.t_router() + cfg.t_link() + cfg.clk * lt_extra_cycles;

    if (ex) {
      ++ex->hops;
      ex->in_port = noc::opposite(op);
      noc::FlitRecord r;
      r.kind = noc::FlitRecord::R_HOP;
      r.pkt_id = ex->pkt_id;
      r.hops = ex->hops;
      r.inject_time = ex->inject_time;
      r.now = sc_core::sc_time_stamp() + delay;   // delay 尚未被同步，只能作估计
      stat_port.write(r);
    }
    ++n_forwarded;

    trans.set_response_status(tlm::TLM_OK_RESPONSE);
    (*out[op])->b_transport(trans, delay);           // 阻塞式，一条调用链走到底
  }

  unsigned int dbg_fw(int /*bind*/, gp_t& trans)
  {
    // LT 模型没有真实的 FIFO，debug 读只能返回计数快照，不能 pop 任何东西
    unsigned char* p = trans.get_data_ptr();
    if (!p || trans.get_data_length() < 4) return 0;
    const unsigned long v = n_forwarded;
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xff);
    return 4;
  }

  bool dmi_fw(int /*bind*/, gp_t& trans, tlm::tlm_dmi& /*dmi*/)
  {
    trans.set_dmi_allowed(false);
    return false;   // 互连不提供 DMI 直访
  }

  void invalidate_dmi(int /*port*/, sc_dt::uint64 /*s*/, sc_dt::uint64 /*e*/) {}
};

#endif // ROUTER_LT_H
