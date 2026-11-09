// ============================================================================
// noc_top.cpp -- 4x4 2D-mesh 顶层：把统一的 router 模型拼成不同 NoC
//
//   ./noc_demo at 4 4 20   -> AT 模型
//   ./noc_demo lt 4 4 20   -> LT 模型（对照）
//
// 这里同时演示了 TLM 剩下的几类接口：
//   * DMI / transport_dbg / invalidate_direct_mem_ptr（NetworkSink）
//   * tlm_analysis_port / tlm_analysis_if（零副作用的性能旁路）
//   * tlm_global_quantum + tlm_quantumkeeper（LT 的时间解耦）
//   * TLM_COMPLETED 早完成（本地 ejection / NullSink）
// ============================================================================
#include "noc_common.h"
#include "router_at.h"
#include "router_lt.h"

#include <iostream>
#include <string>
#include <vector>
#include <memory>

static unsigned char g_dummy_data[8] = {0xA5, 0x5A, 0x01, 0x02, 0, 0, 0, 0};

static tlm::tlm_generic_payload* make_flit(noc::flit_mm& mm,
                                           const noc::RouterConfig& cfg,
                                           int src, int dst, int pkt_id,
                                           noc::FlitKind kind, int vc)
{
  tlm::tlm_generic_payload* t = mm.allocate();
  t->set_command(tlm::TLM_WRITE_COMMAND);          // flit 是一次前向写事务
  t->set_address(static_cast<sc_dt::uint64>(dst)); // address 编码目的节点
  t->set_data_ptr(g_dummy_data);                   // 性能模型不关心数据内容
  t->set_data_length(4);
  t->set_streaming_width(4);
  t->set_byte_enable_ptr(nullptr);
  t->set_byte_enable_length(0);
  t->set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
  t->set_dmi_allowed(false);

  noc::flit_ext* ex = new noc::flit_ext;           // auto extension：payload 负责释放
  ex->kind = kind;
  ex->pkt_id = pkt_id;
  ex->src = src;
  ex->dst = dst;
  ex->hops = 0;
  ex->inject_time = sc_core::sc_time_stamp();
  ex->in_port = noc::P_L;                          // 从本地端口注入
  ex->in_vc = vc;
  t->set_auto_extension(ex);
  return t;
}

static int pick_dst(int src, int pkt, int n_nodes, int seed)
{
  if (n_nodes <= 1) return 0;
  return (src + 1 + (pkt * 7 + seed * 13) % (n_nodes - 1)) % n_nodes;
}

// ---------------------------------------------------------------------------
// instance-specific extension：只有「创建它的那个 accessor」看得见。
// 注入端用它记住每个 flit 用的是哪个注入 VC —— 这个字段是链路私有的，
// 不能放进 ignorable extension，因为 router 每跳都会重写 flit_ext::in_vc，
// 等 BEGIN_RESP 回来时已经不是注入时的值了。
// ---------------------------------------------------------------------------
struct inj_ext : tlm_utils::instance_specific_extension<inj_ext> {
  int vc;
  explicit inj_ext(int v) : vc(v) {}
};

// ---------------------------------------------------------------------------
// 性能监视器：实现 tlm_analysis_if，纯旁路、零时序影响
// ---------------------------------------------------------------------------
struct PerfMonitor : sc_core::sc_module, tlm::tlm_analysis_if<noc::FlitRecord>
{
  unsigned long n_eject = 0, n_hop = 0;
  double sum_latency_ns = 0.0, max_latency_ns = 0.0;
  unsigned long sum_hops = 0;
  sc_core::sc_time t_first = sc_core::SC_ZERO_TIME, t_last = sc_core::SC_ZERO_TIME;

  explicit PerfMonitor(sc_core::sc_module_name nm) : sc_core::sc_module(nm) {}

  void write(const noc::FlitRecord& r) override
  {
    if (r.kind == noc::FlitRecord::R_HOP) { ++n_hop; return; }
    if (r.kind != noc::FlitRecord::R_EJECT) return;
    ++n_eject;
    sum_hops += r.hops;
    const double lat = (r.now - r.inject_time).to_seconds() * 1e9;
    sum_latency_ns += lat;
    if (lat > max_latency_ns) max_latency_ns = lat;
    if (t_first == sc_core::SC_ZERO_TIME) t_first = r.now;
    t_last = r.now;
  }

  void report(const std::string& tag) const
  {
    std::cout << "  [" << tag << "] ejected packets(flits) = " << n_eject
              << ", forwarded hops = " << n_hop << "\n";
    if (n_eject) {
      std::cout << "  [" << tag << "] avg latency = "
                << sum_latency_ns / n_eject << " ns, max = "
                << max_latency_ns << " ns, avg hops = "
                << static_cast<double>(sum_hops) / n_eject << "\n";
      const double span = (t_last - t_first).to_seconds() * 1e9;
      if (span > 0)
        std::cout << "  [" << tag << "] throughput = "
                  << n_eject / (span / 1000.0) << " flits/us\n";
    }
  }
};

// ---------------------------------------------------------------------------
// 本地 ejection 端口的 target：演示 DMI / debug / 早完成
// ---------------------------------------------------------------------------
SC_MODULE(NetworkSink)
{
  typedef noc::noc_protocol_types PT;
  tlm_utils::simple_target_socket<NetworkSink, 32, PT> sock;
  tlm::tlm_analysis_port<noc::FlitRecord> stat;
  std::vector<unsigned char> regs;      // 一组「统计寄存器」
  unsigned long n_ejected = 0;

  explicit NetworkSink(sc_core::sc_module_name nm)
    : sc_core::sc_module(nm), sock("sock"), regs(256, 0)
  {
    sock.register_nb_transport_fw(this, &NetworkSink::nb_fw);
    sock.register_b_transport(this, &NetworkSink::b_fw);
    sock.register_transport_dbg(this, &NetworkSink::dbg_fw);
    sock.register_get_direct_mem_ptr(this, &NetworkSink::dmi_fw);
  }

  void record(tlm::tlm_generic_payload& trans, const sc_core::sc_time& d)
  {
    noc::flit_ext* ex = trans.get_extension<noc::flit_ext>();
    ++n_ejected;
    regs[0] = static_cast<unsigned char>(n_ejected & 0xff);
    if (ex) {
      noc::FlitRecord r;
      r.kind = noc::FlitRecord::R_EJECT;
      r.pkt_id = ex->pkt_id;
      r.hops = ex->hops;
      r.inject_time = ex->inject_time;
      r.now = sc_core::sc_time_stamp() + d;
      stat.write(r);
    }
  }

  tlm::tlm_sync_enum nb_fw(tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase,
                           sc_core::sc_time& d)
  {
    if (phase == tlm::BEGIN_REQ) {
      record(trans, d);
      trans.set_response_status(tlm::TLM_OK_RESPONSE);
      return tlm::TLM_COMPLETED;   // early completion：四相握手被合法地短路
    }
    return tlm::TLM_ACCEPTED;
  }

  void b_fw(tlm::tlm_generic_payload& trans, sc_core::sc_time& d)
  {
    d += sc_core::sc_time(1, sc_core::SC_NS);
    record(trans, d);
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
  }

  unsigned int dbg_fw(tlm::tlm_generic_payload& trans)
  {
    unsigned char* p = trans.get_data_ptr();
    if (!p) return 0;
    const unsigned int n = std::min<unsigned int>(trans.get_data_length(),
                                                  static_cast<unsigned int>(regs.size()));
    for (unsigned int i = 0; i < n; ++i) p[i] = regs[i];
    return n;   // 无副作用、不推进仿真时间、不改变 n_ejected
  }

  bool dmi_fw(tlm::tlm_generic_payload& trans, tlm::tlm_dmi& dmi)
  {
    dmi.set_dmi_ptr(regs.data());
    dmi.set_start_address(0);
    dmi.set_end_address(regs.size() - 1);
    dmi.set_read_latency(sc_core::sc_time(1, sc_core::SC_NS));
    dmi.set_write_latency(sc_core::sc_time(1, sc_core::SC_NS));
    dmi.allow_read_write();
    trans.set_dmi_allowed(true);
    return true;
  }
};

// ---------------------------------------------------------------------------
// 边界端口的兜底 target（XY 路由在 mesh 内不会用到，仅保证每个 socket 有绑定）
// ---------------------------------------------------------------------------
SC_MODULE(NullSink)
{
  typedef noc::noc_protocol_types PT;
  tlm_utils::multi_passthrough_target_socket<NullSink, 32, PT> in;
  unsigned long n_dropped = 0;

  explicit NullSink(sc_core::sc_module_name nm) : sc_core::sc_module(nm), in("in")
  {
    in.register_nb_transport_fw(this, &NullSink::nb_fw);
    in.register_b_transport(this, &NullSink::b_fw);
  }
  tlm::tlm_sync_enum nb_fw(int, tlm::tlm_generic_payload& trans, tlm::tlm_phase&,
                           sc_core::sc_time&)
  {
    ++n_dropped;
    trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
    return tlm::TLM_COMPLETED;
  }
  void b_fw(int, tlm::tlm_generic_payload& trans, sc_core::sc_time&) { ++n_dropped; }
};

// ---------------------------------------------------------------------------
// AT 流量发生器：nb_transport_fw 注入，bw 路径上收 BEGIN_RESP
// ---------------------------------------------------------------------------
SC_MODULE(TrafficGenAT)
{
  typedef noc::noc_protocol_types PT;
  tlm_utils::simple_initiator_socket<TrafficGenAT, 32, PT> sock;
  noc::flit_mm* mm;
  noc::RouterConfig cfg;
  int src = 0, seed = 0;
  tlm_utils::instance_specific_extension_accessor acc;
  std::vector<int> inj_credit;   // NI 侧对本地注入 VC 的 credit（= 下游空槽数）

  SC_HAS_PROCESS(TrafficGenAT);
  TrafficGenAT(sc_core::sc_module_name nm, noc::flit_mm* m, noc::RouterConfig c, int s)
    : sc_core::sc_module(nm), sock("sock"), mm(m), cfg(c), src(s), seed(s),
      inj_credit(c.n_vcs, c.buf_depth)
  {
    sock.register_nb_transport_bw(this, &TrafficGenAT::nb_bw);
    sock.register_invalidate_direct_mem_ptr(this, &TrafficGenAT::invalidate_dmi);
    SC_THREAD(run);
  }

  void run()
  {
    const int n_nodes = cfg.mesh_x * cfg.mesh_y;
    for (int pkt = 0; pkt < cfg.n_packets; ++pkt) {
      const int dst = pick_dst(src, pkt, n_nodes, seed);
      const int vc  = pkt % cfg.n_vcs;
      for (int f = 0; f < cfg.packet_len; ++f) {
        noc::FlitKind kind = (cfg.packet_len == 1) ? noc::F_HEAD_TAIL
                           : (f == 0) ? noc::F_HEAD
                           : (f == cfg.packet_len - 1) ? noc::F_TAIL : noc::F_BODY;
        // NI 必须遵守 credit：没有空槽就不能注入，这是真实 NoC 的注入反压
        while (inj_credit[vc] <= 0) wait(cfg.clk);
        tlm::tlm_generic_payload* t = make_flit(*mm, cfg, src, dst, pkt, kind, vc);
        acc(*t).set_extension(new inj_ext(vc));
        --inj_credit[vc];                  // 发出一个 flit，扣一个 credit
        t->acquire();                       // 生成者持有一份引用
        tlm::tlm_phase ph(tlm::BEGIN_REQ);
        sc_core::sc_time d = cfg.clk;
        tlm::tlm_sync_enum s = sock->nb_transport_fw(*t, ph, d);
        if (s == tlm::TLM_COMPLETED) {
          ++inj_credit[vc];                // 被拒绝，把 credit 还回来
          t->release();
        }
        wait(cfg.clk * cfg.inject_gap_cycles);
      }
      wait(cfg.clk * cfg.inject_gap_cycles);
    }
  }

  tlm::tlm_sync_enum nb_bw(tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase,
                           sc_core::sc_time&)
  {
    if (phase == tlm::BEGIN_RESP) {
      inj_ext* e = nullptr;
      acc(trans).get_extension(e);
      if (e) {
        ++inj_credit[e->vc];              // 下游消费了 flit，归还 credit
        acc(trans).clear_extension(e);
      }
      tlm::tlm_phase p(tlm::END_RESP);
      sc_core::sc_time z = sc_core::SC_ZERO_TIME;
      sock->nb_transport_fw(trans, p, z);
      trans.release();                    // 释放生成者的引用
      return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
  }

  void invalidate_dmi(sc_dt::uint64, sc_dt::uint64) { /* DMI 被下游撤销时的回调 */ }
};

// ---------------------------------------------------------------------------
// LT 流量发生器：b_transport + quantum keeper（temporal decoupling）
// ---------------------------------------------------------------------------
SC_MODULE(TrafficGenLT)
{
  typedef noc::noc_protocol_types PT;
  tlm_utils::simple_initiator_socket<TrafficGenLT, 32, PT> sock;
  tlm_utils::tlm_quantumkeeper qk;
  noc::flit_mm* mm;
  noc::RouterConfig cfg;
  int src = 0, seed = 0;

  SC_HAS_PROCESS(TrafficGenLT);
  TrafficGenLT(sc_core::sc_module_name nm, noc::flit_mm* m, noc::RouterConfig c, int s)
    : sc_core::sc_module(nm), sock("sock"), mm(m), cfg(c), src(s), seed(s)
  {
    sock.register_invalidate_direct_mem_ptr(this, &TrafficGenLT::invalidate_dmi);
    SC_THREAD(run);
  }

  void run()
  {
    const int n_nodes = cfg.mesh_x * cfg.mesh_y;
    qk.reset();
    tlm_utils::tlm_quantumkeeper::set_global_quantum(sc_core::sc_time(100, sc_core::SC_NS));
    for (int pkt = 0; pkt < cfg.n_packets; ++pkt) {
      const int dst = pick_dst(src, pkt, n_nodes, seed);
      for (int f = 0; f < cfg.packet_len; ++f) {
        noc::FlitKind kind = (cfg.packet_len == 1) ? noc::F_HEAD_TAIL
                           : (f == 0) ? noc::F_HEAD
                           : (f == cfg.packet_len - 1) ? noc::F_TAIL : noc::F_BODY;
        tlm::tlm_generic_payload* t = make_flit(*mm, cfg, src, dst, pkt, kind, 0);
        // LT 用了 temporal decoupling：sc_time_stamp() 落后于本地时间，
        // 注入时刻必须写成「同步基准 + 本地偏移」，延迟统计才不会失真
        t->get_extension<noc::flit_ext>()->inject_time =
            sc_core::sc_time_stamp() + qk.get_local_time();
        t->acquire();
        sc_core::sc_time d = qk.get_local_time();
        sock->b_transport(*t, d);          // 一次调用走完整条路径
        t->release();                      // 同步完成，没有 bw 回调
        d += cfg.clk * cfg.inject_gap_cycles;
        qk.set(d);
        if (qk.need_sync()) qk.sync();     // 只在 quantum 边界真正 wait
      }
    }
    qk.sync();
  }

  void invalidate_dmi(sc_dt::uint64, sc_dt::uint64) {}
};

// ---------------------------------------------------------------------------
// 统一的连线函数：同一个 router 类拼出 mesh / torus / ring（这里演示 mesh）
// ---------------------------------------------------------------------------
template <typename R>
static void bind_router(R* r, int id, int p, const std::vector<R*>& all,
                        NetworkSink* sink, NullSink* null_sink,
                        const noc::RouterConfig& cfg)
{
  if (p == noc::P_L) { r->out[p]->bind(sink->sock); return; }
  int nid = -1;
  const int x = id % cfg.mesh_x, y = id / cfg.mesh_x;
  switch (p) {
    case noc::P_N: if (y > 0)              nid = noc::xy2id(x, y - 1, cfg.mesh_x); break;
    case noc::P_S: if (y < cfg.mesh_y - 1) nid = noc::xy2id(x, y + 1, cfg.mesh_x); break;
    case noc::P_W: if (x > 0)              nid = noc::xy2id(x - 1, y, cfg.mesh_x); break;
    case noc::P_E: if (x < cfg.mesh_x - 1) nid = noc::xy2id(x + 1, y, cfg.mesh_x); break;
    default: break;
  }
  if (nid >= 0) r->out[p]->bind(all[nid]->in);
  else          r->out[p]->bind(null_sink->in);
}

// ---------------------------------------------------------------------------
int sc_main(int argc, char* argv[])
{
  const std::string mode = (argc > 1) ? argv[1] : "at";
  const bool use_lt = (mode == "lt");
  const int mx = (argc > 2) ? std::atoi(argv[2]) : 4;
  const int my = (argc > 3) ? std::atoi(argv[3]) : 4;
  const int np = (argc > 4) ? std::atoi(argv[4]) : 20;
  const int gap = (argc > 5) ? std::atoi(argv[5]) : 1;
  const int n_nodes = mx * my;

  noc::RouterConfig base;
  base.mesh_x = mx; base.mesh_y = my; base.n_packets = np;
  base.inject_gap_cycles = gap;

  noc::flit_mm mm;
  sc_core::sc_clock clk("clk", base.clk);
  NullSink null_sink("null_sink");
  PerfMonitor monitor("monitor");

  std::vector<NetworkSink*> sinks(n_nodes);
  std::vector<RouterAT*> at(n_nodes, nullptr);
  std::vector<RouterLT*> lt(n_nodes, nullptr);
  std::vector<TrafficGenAT*> gen_at(n_nodes, nullptr);
  std::vector<TrafficGenLT*> gen_lt(n_nodes, nullptr);

  for (int i = 0; i < n_nodes; ++i) {
    noc::RouterConfig c = base;
    c.id = i; noc::id2xy(i, mx, c.x, c.y);
    sinks[i] = new NetworkSink((std::string("sink_") + std::to_string(i)).c_str());
    sinks[i]->stat.bind(monitor);
    if (use_lt) {
      lt[i] = new RouterLT((std::string("router_") + std::to_string(i)).c_str(), c);
      lt[i]->stat_port.bind(monitor);
      gen_lt[i] = new TrafficGenLT((std::string("gen_") + std::to_string(i)).c_str(), &mm, c, i);
      gen_lt[i]->sock.bind(lt[i]->in);
    } else {
      at[i] = new RouterAT((std::string("router_") + std::to_string(i)).c_str(), c, &mm);
      at[i]->clk(clk);
      at[i]->stat_port.bind(monitor);
      gen_at[i] = new TrafficGenAT((std::string("gen_") + std::to_string(i)).c_str(), &mm, c, i);
      gen_at[i]->sock.bind(at[i]->in);
    }
  }
  for (int i = 0; i < n_nodes; ++i) {
    for (int p = 0; p < noc::N_PORTS; ++p) {
      if (use_lt) bind_router(lt[i], i, p, lt, sinks[i], &null_sink, base);
      else        bind_router(at[i], i, p, at, sinks[i], &null_sink, base);
    }
  }

  // ---- 演示 DMI / debug transport（不推进仿真时间，也不改变 FIFO 状态） ----
  {
    unsigned char buf[8] = {0};
    tlm::tlm_generic_payload probe;
    probe.set_command(tlm::TLM_READ_COMMAND);
    probe.set_address(0);
    probe.set_data_ptr(buf);
    probe.set_data_length(8);
    tlm::tlm_fw_transport_if<noc::noc_protocol_types>& fw =
        sinks[0]->sock.get_base_interface();
    const unsigned int got = fw.transport_dbg(probe);
    tlm::tlm_dmi dmi;
    const bool dmi_ok = fw.get_direct_mem_ptr(probe, dmi);
    std::cout << "[TLM] transport_dbg read " << got
              << " bytes, DMI granted=" << (dmi_ok ? "yes" : "no") << "\n";
    sinks[0]->sock->invalidate_direct_mem_ptr(0, 255);  // 触发 bw 侧回调
  }

  std::cout << "=== NoC " << mx << "x" << my << " | style=" << mode
            << " | packets/node=" << np << " ===\n";
  const sc_core::sc_time t0 = sc_core::sc_time_stamp();
  sc_start(sc_core::sc_time(200, sc_core::SC_US));
  const sc_core::sc_time sim_span = sc_core::sc_time_stamp() - t0;

  monitor.report(mode);
  std::cout << "  wall time simulated = " << sim_span << "\n";
  std::cout << "  dropped at boundary = " << null_sink.n_dropped << "\n";
  if (!use_lt) {
    unsigned long stalled = 0, conflicts = 0, va_stalls = 0;
    for (int i = 0; i < n_nodes; ++i) {
      stalled   += at[i]->n_credit_stall;
      conflicts += at[i]->n_sa_conflict;
      va_stalls += at[i]->n_va_stall;
    }
    std::cout << "  [AT only] credit stalls=" << stalled
              << ", SA conflicts=" << conflicts
              << ", VA stalls=" << va_stalls << "\n";
  }
  std::cout << "=== done ===\n";
  return 0;
}
