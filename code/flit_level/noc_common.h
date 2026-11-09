// ============================================================================
// noc_common.h -- TLM-2.0 NoC 性能模型：事务类型、扩展、相位、配置、公共工具
// 目标平台：SystemC 2.3.4 / IEEE 1666-2011 (TLM-2.0)
// ============================================================================
#ifndef NOC_COMMON_H
#define NOC_COMMON_H

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/multi_passthrough_target_socket.h>
#include <tlm_utils/peq_with_cb_and_phase.h>
#include <tlm_utils/peq_with_get.h>
#include <tlm_utils/tlm_quantumkeeper.h>
#include <tlm_utils/instance_specific_extensions.h>

#include <deque>
#include <vector>
#include <cstdint>

namespace noc {

// ---------------------------------------------------------------------------
// 1) 自定义 protocol types
//    TLM-2.0 base protocol 规定「每个 socket 只允许一个未完成的 request /
//    response」。NoC 里同一条链路上会同时有几十个 in-flight flit，必须让
//    多个事务并发。标准允许显式声明一个新的 protocol types class 来脱离
//    base protocol 规则（LRM 给出的三种做法之一），这里即使 payload 和
//    phase 的类型都不变，也要写出来。
// ---------------------------------------------------------------------------
struct noc_protocol_types {
  typedef tlm::tlm_generic_payload tlm_payload_type;
  typedef tlm::tlm_phase           tlm_phase_type;
};

// ---------------------------------------------------------------------------
// 2) 扩展相位（extended phases）
//    标准四相：UNINITIALIZED_PHASE=0, BEGIN_REQ=1, END_REQ=2,
//              BEGIN_RESP=3, END_RESP=4
//    TLM_DECLARE_EXTENDED_PHASE 通过 typeid 在全局 phase registry 注册，
//    跨 translation unit 拿到的是同一个 ID，比手写 tlm_phase(5) 安全。
//    前三个用于 router 内部流水线的 PEQ 调度，CREDIT 用于反向路径的
//    credit 归还事件。
// ---------------------------------------------------------------------------
TLM_DECLARE_EXTENDED_PHASE(RC_DONE);    // 路由计算完成
TLM_DECLARE_EXTENDED_PHASE(VA_REQ);     // 请求分配输出 VC
TLM_DECLARE_EXTENDED_PHASE(VA_GRANT);   // 输出 VC 分配成功
TLM_DECLARE_EXTENDED_PHASE(ST_DONE);    // crossbar 穿越完成
TLM_DECLARE_EXTENDED_PHASE(CREDIT);     // credit 归还（时间事件）

// ---------------------------------------------------------------------------
// 3) 端口与 flit 类型（对应 5 端口 2D-mesh router）
// ---------------------------------------------------------------------------
enum Port { P_N = 0, P_E = 1, P_S = 2, P_W = 3, P_L = 4, N_PORTS = 5 };

inline int opposite(int p) {
  switch (p) {
    case P_N: return P_S;  case P_S: return P_N;
    case P_E: return P_W;  case P_W: return P_E;
    default:  return P_L;
  }
}

enum FlitKind { F_HEAD = 0, F_BODY = 1, F_TAIL = 2, F_HEAD_TAIL = 3 };
inline bool is_head(FlitKind k) { return k == F_HEAD || k == F_HEAD_TAIL; }
inline bool is_tail(FlitKind k) { return k == F_TAIL || k == F_HEAD_TAIL; }

inline int  xy2id(int x, int y, int mx)      { return y * mx + x; }
inline void id2xy(int id, int mx, int& x, int& y) { x = id % mx; y = id / mx; }

// 确定性 XY 路由（先走 X 再走 Y）；到达本节点返回 P_L（ejection）
inline int route_xy(int cx, int cy, int dx, int dy) {
  if (dx > cx) return P_E;
  if (dx < cx) return P_W;
  if (dy > cy) return P_S;
  if (dy < cy) return P_N;
  return P_L;
}
inline int route_to_id(int cx, int cy, int dst, int mx) {
  int dx, dy; id2xy(dst, mx, dx, dy);
  return route_xy(cx, cy, dx, dy);
}

// ---------------------------------------------------------------------------
// 4) Router 参数：全部来自真实 spec（见文章 §1），可参数化出不同 NoC
// ---------------------------------------------------------------------------
struct RouterConfig {
  int id = 0, x = 0, y = 0;
  int mesh_x = 4, mesh_y = 4;
  int n_ports  = N_PORTS;   // radix：5 端口 (N,E,S,W,Local)
  int n_vcs    = 2;         // num_vcs
  int buf_depth = 4;        // vc_buf_size（每个 VC 的 FIFO 深度，单位 flit）
  int packet_len = 4;       // const_flits_per_packet
  int n_packets = 200;

  // 流水线延迟（单位：时钟周期），对应 BookSim2 的
  // routing_delay / vc_alloc_delay / sw_alloc_delay / st_final_delay /
  // credit_delay / channel latency
  int rc_cycles = 1, va_cycles = 1, sa_cycles = 1,
      st_cycles = 1, credit_cycles = 1, link_cycles = 1;
  int inject_gap_cycles = 1;   // 注入间隔

  sc_core::sc_time clk = sc_core::sc_time(1, sc_core::SC_NS);

  sc_core::sc_time t_rc()     const { return clk * rc_cycles; }
  sc_core::sc_time t_va()     const { return clk * va_cycles; }
  sc_core::sc_time t_sa()     const { return clk * sa_cycles; }
  sc_core::sc_time t_st()     const { return clk * st_cycles; }
  sc_core::sc_time t_credit() const { return clk * credit_cycles; }
  sc_core::sc_time t_link()   const { return clk * link_cycles; }
  sc_core::sc_time t_router() const {
    return clk * (rc_cycles + va_cycles + sa_cycles + st_cycles);
  }
};

// ---------------------------------------------------------------------------
// 5) ignorable extension：一个 flit 的全部元数据
//    端到端字段（pkt_id/src/dst/inject_time/hops）在整条路径上不变；
//    逐跳字段（in_port/in_vc/out_port/out_vc）每过一跳被重写。
// ---------------------------------------------------------------------------
struct flit_ext : tlm::tlm_extension<flit_ext> {
  FlitKind kind = F_HEAD;
  int pkt_id = 0, src = 0, dst = 0, hops = 0;
  sc_core::sc_time inject_time = sc_core::SC_ZERO_TIME;

  int in_port = P_L;   // 本跳：到达端口
  int in_vc   = 0;     // 本跳：到达 VC
  int out_port = -1;   // 本跳：RC 结果
  int out_vc   = -1;   // 本跳：VA 结果

  tlm::tlm_extension_base* clone() const override { return new flit_ext(*this); }
  void copy_from(tlm::tlm_extension_base const& e) override {
    *this = static_cast<flit_ext const&>(e);
  }
};

// ---------------------------------------------------------------------------
// 6) memory manager + payload 池：util 层的引用计数机制
// ---------------------------------------------------------------------------
class flit_mm : public tlm::tlm_mm_interface {
  std::vector<tlm::tlm_generic_payload*> pool_;
public:
  tlm::tlm_generic_payload* allocate() {
    if (pool_.empty()) return new tlm::tlm_generic_payload(this);
    tlm::tlm_generic_payload* p = pool_.back();
    pool_.pop_back();
    return p;
  }
  void free(tlm::tlm_generic_payload* p) override {
    p->free_all_extensions();          // 释放 set_auto_extension 挂上的扩展
    p->set_command(tlm::TLM_IGNORE_COMMAND);
    p->set_address(0);
    p->set_data_ptr(nullptr);
    p->set_data_length(0);
    p->set_byte_enable_ptr(nullptr);
    p->set_byte_enable_length(0);
    p->set_streaming_width(0);
    p->set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
    p->set_dmi_allowed(false);
    pool_.push_back(p);
  }
  ~flit_mm() override { for (auto* p : pool_) delete p; }
};

// 性能监控记录（通过 tlm_analysis_port 旁路输出，不占事务通路）
struct FlitRecord {
  enum Kind { R_HOP = 0, R_EJECT = 1, R_STALL = 2 } kind = R_HOP;
  int pkt_id = 0, hops = 0;
  sc_core::sc_time inject_time = sc_core::SC_ZERO_TIME;
  sc_core::sc_time now = sc_core::SC_ZERO_TIME;
};

// 每个 flit 在每个 router 上的「链路上下文」。
// 之所以不直接读 payload 里的 in_port/in_vc：那个 payload 在发给下一跳后会
// 被下一跳立刻改写，等反向的 BEGIN_RESP 回来时字段已经不是本跳的值了。
struct LinkCtx { int port = -1; int vc = -1; int bind = -1; };

} // namespace noc
#endif // NOC_COMMON_H
