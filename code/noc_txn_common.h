// ============================================================================
// noc_txn_common.h -- 架构探索用 transaction 级 NoC 模型：公共类型
//
// 建模层级：一条完整消息 = 一个 tlm_generic_payload，不做逐片切分。
//   * 竞争不下沉到流水线，上移到「输出链路 = 单服务器」的服务模型
//   * 扩展相位只有 LINK_DONE / CREDIT 两个
//   * 逐跳状态不放在 payload extension 里，放在 router 自己的 HopCtx 表
//   * AT 用 credit 做流控（NI 硬反压），LT 用 ready/valid 握手（无反压）
//
// 目标平台：SystemC 2.3.4 / IEEE 1666-2011 (TLM-2.0)，C++14
// ============================================================================
#ifndef NOC_TXN_COMMON_H
#define NOC_TXN_COMMON_H

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/multi_passthrough_target_socket.h>
#include <tlm_utils/peq_with_cb_and_phase.h>
#include <tlm_utils/peq_with_get.h>
#include <tlm_utils/tlm_quantumkeeper.h>
#include <tlm_utils/instance_specific_extensions.h>

#include <string>
#include <vector>
#include <cstdlib>
#include <cstdint>
#include <cmath>

namespace noc {

// ---------------------------------------------------------------------------
// 1) 自定义 protocol types
//    base protocol 规定每个 socket 只允许一个未完成 request/response；
//    NoC 链路上同时有多条 in-flight 消息，必须显式脱离该规则。
// ---------------------------------------------------------------------------
struct noc_protocol_types {
  typedef tlm::tlm_generic_payload tlm_payload_type;
  typedef tlm::tlm_phase           tlm_phase_type;
};

// ---------------------------------------------------------------------------
// 2) 扩展相位：只剩两个，都服务于 AT 的 credit 流控
//    LINK_DONE = 本跳链路服务完成，此刻释放输入槽并把 credit 还给上游
//    CREDIT    = 反向路径上「一个 message slot 已归还」的显式事件
// ---------------------------------------------------------------------------
TLM_DECLARE_EXTENDED_PHASE(LINK_DONE);
TLM_DECLARE_EXTENDED_PHASE(CREDIT);

// ---------------------------------------------------------------------------
// 3) 端口、拓扑、路由、流量模式
// ---------------------------------------------------------------------------
enum Port { P_N = 0, P_E = 1, P_S = 2, P_W = 3, P_L = 4, N_PORTS = 5 };

inline int opposite(int p) {
  switch (p) {
    case P_N: return P_S;  case P_S: return P_N;
    case P_E: return P_W;  case P_W: return P_E;
    default:  return P_L;
  }
}

enum Topology { TOPO_MESH = 0, TOPO_TORUS = 1, TOPO_RING = 2 };
enum Routing  { RT_XY = 0, RT_YX = 1, RT_OBLIVIOUS = 2 };
enum Traffic  { TR_UNIFORM = 0, TR_TRANSPOSE = 1, TR_BITCOMP = 2, TR_HOTSPOT = 3 };

inline Topology parse_topology(const std::string& s) {
  if (s == "torus") return TOPO_TORUS;
  if (s == "ring")  return TOPO_RING;
  return TOPO_MESH;
}
inline Routing parse_routing(const std::string& s) {
  if (s == "yx")        return RT_YX;
  if (s == "oblivious") return RT_OBLIVIOUS;
  return RT_XY;
}
inline Traffic parse_traffic(const std::string& s) {
  if (s == "transpose") return TR_TRANSPOSE;
  if (s == "bitcomp")   return TR_BITCOMP;
  if (s == "hotspot")   return TR_HOTSPOT;
  return TR_UNIFORM;
}
inline const char* topo_name(Topology t) {
  return t == TOPO_TORUS ? "torus" : (t == TOPO_RING ? "ring" : "mesh");
}
inline const char* routing_name(Routing r) {
  return r == RT_YX ? "yx" : (r == RT_OBLIVIOUS ? "oblivious" : "xy");
}
inline const char* traffic_name(Traffic t) {
  switch (t) {
    case TR_TRANSPOSE: return "transpose";
    case TR_BITCOMP:   return "bitcomp";
    case TR_HOTSPOT:   return "hotspot";
    default:           return "uniform";
  }
}

inline int  xy2id(int x, int y, int mx) { return y * mx + x; }
inline void id2xy(int id, int mx, int& x, int& y) { x = id % mx; y = id / mx; }

// 路由计算：返回本节点应走的输出端口。
// torus 用环绕方向的最短路；ring 只有 x 维；oblivious 按消息随机选 XY 或 YX。
inline int route_port(Topology topo, Routing rt,
                      int cx, int cy, int dx, int dy,
                      int mx, int my, int choice)
{
  if (topo == TOPO_RING) {
    if (dx == cx) return P_L;
    const int d = dx - cx;
    if (d > 0) return (d <= mx / 2) ? P_E : P_W;
    return (-d <= mx / 2) ? P_W : P_E;
  }

  int ex = dx - cx, ey = dy - cy;
  if (topo == TOPO_TORUS) {
    if (ex >  mx / 2) ex -= mx; else if (ex < -mx / 2) ex += mx;
    if (ey >  my / 2) ey -= my; else if (ey < -my / 2) ey += my;
  }
  const bool x_first = (rt == RT_XY) ||
                       (rt == RT_OBLIVIOUS && (choice & 1) == 0);
  if (x_first) {
    if (ex > 0) return P_E;
    if (ex < 0) return P_W;
    if (ey > 0) return P_S;
    if (ey < 0) return P_N;
  } else {
    if (ey > 0) return P_S;
    if (ey < 0) return P_N;
    if (ex > 0) return P_E;
    if (ex < 0) return P_W;
  }
  return P_L;
}

// 端口 p 的邻居 id；未连接返回 -1（绑定到 NullSink）
inline int neighbor(Topology topo, int x, int y, int p, int mx, int my)
{
  switch (p) {
    case P_N:
      if (topo == TOPO_TORUS) return xy2id(x, (y - 1 + my) % my, mx);
      return (y > 0) ? xy2id(x, y - 1, mx) : -1;
    case P_S:
      if (topo == TOPO_TORUS) return xy2id(x, (y + 1) % my, mx);
      return (y < my - 1) ? xy2id(x, y + 1, mx) : -1;
    case P_E:
      if (topo == TOPO_TORUS || topo == TOPO_RING) return xy2id((x + 1) % mx, y, mx);
      return (x < mx - 1) ? xy2id(x + 1, y, mx) : -1;
    case P_W:
      if (topo == TOPO_TORUS || topo == TOPO_RING) return xy2id((x - 1 + mx) % mx, y, mx);
      return (x > 0) ? xy2id(x - 1, y, mx) : -1;
    default: return -1;
  }
}

// 流量模式：架构探索主要靠换 traffic 来看拓扑/路由的鲁棒性
inline int pick_dst(Traffic tr, int src, int msg, int mx, int my, int seed)
{
  const int n = mx * my;
  if (n <= 1) return 0;
  switch (tr) {
    case TR_TRANSPOSE: {
      int x, y; id2xy(src, mx, x, y);
      return xy2id(y % mx, x % my, mx);
    }
    case TR_BITCOMP: {
      if ((n & (n - 1)) != 0) break;             // 非 2 的幂时退回均匀
      int bits = 0; while ((1 << bits) < n) ++bits;
      int v = 0;
      for (int b = 0; b < bits; ++b)
        if (!(src & (1 << b))) v |= 1 << (bits - 1 - b);
      return v % n;
    }
    case TR_HOTSPOT: {
      if (((msg + seed) & 3) == 0) return 0;      // 25% 流量打向 0 号节点
      break;
    }
    default: break;
  }
  return (src + 1 + (msg * 7 + seed * 13) % (n - 1)) % n;
}

// ---------------------------------------------------------------------------
// 4) 架构参数：全部是可扫描的 design knob
//    微架构旋钮（缓冲深度、分配器延迟等）在这个层级被抽象掉，
//    取而代之的是「链路带宽 + 消息长度」这一对服务需求参数。
// ---------------------------------------------------------------------------
struct RouterConfig {
  int id = 0, x = 0, y = 0;
  int mesh_x = 4, mesh_y = 4;
  int n_ports = N_PORTS;
  Topology topology = TOPO_MESH;
  Routing  routing  = RT_XY;
  Traffic  traffic  = TR_UNIFORM;

  // AT 的 credit 池大小：NI 最多允许同时有多少条消息未完成。
  // 注入端是硬反压；中间 router 采用排队网络常见的「缓冲无硬上限」假设，
  // 饱和点由链路服务率决定，credit 在中间节点只做观测统计。
  int ni_window = 8;
  int n_messages = 30;    // 每节点注入的消息数
  int msg_bytes  = 64;    // 默认消息长度（bytes）

  double link_bw = 16.0;  // 链路带宽，bytes/ns（16 B/ns = 128 Gbps）
  int rc_cycles = 1, credit_cycles = 1, link_cycles = 1;
  int inject_gap_cycles = 8;

  sc_core::sc_time clk = sc_core::sc_time(1, sc_core::SC_NS);

  sc_core::sc_time t_rc()     const { return clk * rc_cycles; }
  sc_core::sc_time t_credit() const { return clk * credit_cycles; }
  sc_core::sc_time t_link()   const { return clk * link_cycles; }
  sc_core::sc_time t_serialize(int bytes) const {
    return sc_core::sc_time(static_cast<double>(bytes) / link_bw, sc_core::SC_NS);
  }
  sc_core::sc_time t_service(int bytes) const {
    return t_link() + t_serialize(bytes);
  }
  // 单条链路的理论服务率（messages/ns）
  double link_rate(int bytes) const {
    return 1.0 / t_service(bytes).to_seconds() * 1e-9;
  }
};

// ---------------------------------------------------------------------------
// 5) ignorable extension：一条消息的端到端元数据
//    逐跳字段只有 in_port / out_port（进入下一跳前由上游重写）。
//    其它逐跳状态一律放在 router 的 HopCtx 表里，避免 payload 被下游改写后
//    反向 credit 回来时读到错值。
// ---------------------------------------------------------------------------
struct txn_ext : tlm::tlm_extension<txn_ext> {
  int msg_id = 0, src = 0, dst = 0, hops = 0;
  int msg_bytes = 64;
  int priority = 0;
  int rt_choice = 0;
  int vnet = 0;
  sc_core::sc_time inject_time = sc_core::SC_ZERO_TIME;

  int in_port  = P_L;
  int out_port = -1;

  tlm::tlm_extension_base* clone() const override { return new txn_ext(*this); }
  void copy_from(tlm::tlm_extension_base const& e) override {
    *this = static_cast<txn_ext const&>(e);
  }
};

// ---------------------------------------------------------------------------
// 5b) LT 的 ready/valid 握手：一次事务 = 一次握手
//     AT 用 credit 预约缓冲（有反压）；LT 没有队列，下游对每次有效请求
//     立即给出 ready，因此没有重试、没有信用计数。这正是「只验证
//     连通性与带宽账」所需要的轻量级协议。
// ---------------------------------------------------------------------------
struct rv_ext : tlm::tlm_extension<rv_ext> {
  bool valid = false;                 // 上游：本次事务有数据要发
  bool ready = false;                 // 下游：本跳可以接收
  unsigned int stalls = 0;            // 因 ready=0 重试的次数（LT 恒 0）
  sc_core::sc_time handshake_at = sc_core::SC_ZERO_TIME;

  tlm::tlm_extension_base* clone() const override { return new rv_ext(*this); }
  void copy_from(tlm::tlm_extension_base const& e) override {
    *this = static_cast<rv_ext const&>(e);
  }
};

// ---------------------------------------------------------------------------
// 6) memory manager + payload 池
// ---------------------------------------------------------------------------
class txn_mm : public tlm::tlm_mm_interface {
  std::vector<tlm::tlm_generic_payload*> pool_;
public:
  tlm::tlm_generic_payload* allocate() {
    if (pool_.empty()) return new tlm::tlm_generic_payload(this);
    tlm::tlm_generic_payload* p = pool_.back();
    pool_.pop_back();
    return p;
  }
  void free(tlm::tlm_generic_payload* p) override {
    p->free_all_extensions();
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
  ~txn_mm() override { for (auto* p : pool_) delete p; }
};

// ---------------------------------------------------------------------------
// 7) 旁路统计记录 / 每个 router 的逐事务链路上下文
// ---------------------------------------------------------------------------
struct TxnRecord {
  enum Kind { R_HOP = 0, R_EJECT = 1, R_QUEUE = 2 } kind = R_HOP;
  int msg_id = 0, hops = 0;
  sc_core::sc_time inject_time = sc_core::SC_ZERO_TIME;
  sc_core::sc_time now = sc_core::SC_ZERO_TIME;
  double wait_ns = 0.0;
};

struct HopCtx {
  int port = -1;                 // 输入端口
  int bind = -1;                 // multi target socket 的绑定下标（bw 路径用）
  int out_port = -1;             // 本跳路由结果
  sc_core::sc_time arrival = sc_core::SC_ZERO_TIME;
  sc_core::sc_time done_at = sc_core::SC_ZERO_TIME;
  double wait_ns = 0.0;
};

} // namespace noc
#endif // NOC_TXN_COMMON_H
