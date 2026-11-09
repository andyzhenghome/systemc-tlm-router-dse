// ============================================================================
// noc_txn_top.cpp -- 架构探索顶层：统一的 transaction 级 router 拼成不同 NoC
//
//   ./noc_txn_demo <at|lt> <mx> <my> <msgs/node> <gap_cycles> \
//                  <xy|yx|oblivious> <mesh|torus|ring> \
//                  <uniform|transpose|bitcomp|hotspot> <msg_bytes> <link_bw_B/ns>
//
// 演示的 TLM 接口：四相 nb_transport、时间注解、PEQ、instance-specific
// extension、analysis port、DMI / transport_dbg、TLM_COMPLETED 早完成、
// tlm_global_quantum + tlm_quantumkeeper（LT temporal decoupling）。
// 两套流控协议：AT = credit（NI 硬反压 + 链路服务完成归还）；
// LT = ready/valid（下游恒 ready，只验证连通性与带宽账）。
// ============================================================================
#include "noc_txn_common.h"
#include "router_txn.h"
#include "router_txn_lt.h"

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>

static unsigned char g_dummy_data[8] = {0xA5, 0x5A, 0x01, 0x02, 0, 0, 0, 0};

static tlm::tlm_generic_payload* make_txn(noc::txn_mm& mm,
                                          int src, int dst, int msg_id,
                                          int msg_bytes, int rt_choice)
{
  tlm::tlm_generic_payload* t = mm.allocate();
  t->set_command(tlm::TLM_WRITE_COMMAND);
  t->set_address(static_cast<sc_dt::uint64>(dst));   // 目的节点编码进 address
  t->set_data_ptr(g_dummy_data);                     // 性能模型不关心数据内容
  t->set_data_length(msg_bytes);                     // 长度 = 服务需求，不做逐片切分
  t->set_streaming_width(msg_bytes);
  t->set_byte_enable_ptr(nullptr);
  t->set_byte_enable_length(0);
  t->set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
  t->set_dmi_allowed(false);

  noc::txn_ext* ex = new noc::txn_ext;               // auto extension：payload 负责释放
  ex->msg_id = msg_id;
  ex->src = src; ex->dst = dst;
  ex->msg_bytes = msg_bytes;
  ex->rt_choice = rt_choice;
  ex->inject_time = sc_core::sc_time_stamp();
  ex->in_port = noc::P_L;                            // 本地 NI 注入
  t->set_auto_extension(ex);

  // LT 的 ready/valid：注入端在发起事务的同时置 valid。
  // AT 不使用握手统计，但 payload 带上它不改变任何时序。
  noc::rv_ext* rv = new noc::rv_ext;
  rv->valid = true;
  t->set_auto_extension(rv);
  return t;
}

// instance-specific extension：只有注入端看得见的 in-flight 窗口标签。
// 逐跳字段不能放这里，但「这条消息属于哪个 NI 上下文」是链路私有的，
// 正是这个机制的用途。
struct inj_ext : tlm_utils::instance_specific_extension<inj_ext> {
  int seq;
  explicit inj_ext(int s) : seq(s) {}
};

// ---------------------------------------------------------------------------
// 性能监视器：tlm_analysis_if，纯旁路、零时序影响
// ---------------------------------------------------------------------------
struct PerfMonitor : sc_core::sc_module, tlm::tlm_analysis_if<noc::TxnRecord>
{
  unsigned long n_eject = 0, n_hop = 0, n_queue_sample = 0;
  double sum_latency_ns = 0.0;
  unsigned long sum_hops = 0;
  std::vector<double> lats, waits;
  sc_core::sc_time t_first = sc_core::SC_ZERO_TIME, t_last = sc_core::SC_ZERO_TIME;

  explicit PerfMonitor(sc_core::sc_module_name nm) : sc_core::sc_module(nm) {
    lats.reserve(8192); waits.reserve(8192);
  }

  void write(const noc::TxnRecord& r) override
  {
    if (r.kind == noc::TxnRecord::R_QUEUE) {
      ++n_queue_sample;
      ++n_hop;                       // 每一跳产生一条排队采样
      waits.push_back(r.wait_ns);
      return;
    }
    if (r.kind == noc::TxnRecord::R_HOP) { ++n_hop; return; }
    ++n_eject;
    sum_hops += r.hops;
    const double lat = (r.now - r.inject_time).to_seconds() * 1e9;
    sum_latency_ns += lat;
    lats.push_back(lat);
    if (t_first == sc_core::SC_ZERO_TIME) t_first = r.now;
    t_last = r.now;
  }

  static double percentile(std::vector<double> v, double q, bool nonzero_only = false)
  {
    if (nonzero_only)
      v.erase(std::remove_if(v.begin(), v.end(),
                             [](double x) { return x <= 0.0; }), v.end());
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t i = static_cast<size_t>(q * (v.size() - 1));
    return v[i];
  }

  double avg_latency() const { return n_eject ? sum_latency_ns / n_eject : 0.0; }
  double p95_latency() const { return percentile(lats, 0.95); }
  double max_latency() const { return lats.empty() ? 0.0 : *std::max_element(lats.begin(), lats.end()); }
  double p95_wait() const { return percentile(waits, 0.95, true); }
  double max_wait()  const { return waits.empty() ? 0.0 : *std::max_element(waits.begin(), waits.end()); }
  double span_ns()   const { return (t_last - t_first).to_seconds() * 1e9; }
};

// ---------------------------------------------------------------------------
// 本地 ejection target：DMI / debug / 早完成演示
// ---------------------------------------------------------------------------
SC_MODULE(NetworkSink)
{
  typedef noc::noc_protocol_types PT;
  tlm_utils::simple_target_socket<NetworkSink, 64, PT> sock;
  tlm::tlm_analysis_port<noc::TxnRecord> stat;
  std::vector<unsigned char> regs;
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
    noc::txn_ext* ex = trans.get_extension<noc::txn_ext>();
    ++n_ejected;
    regs[0] = static_cast<unsigned char>(n_ejected & 0xff);
    if (ex) {
      noc::TxnRecord r;
      r.kind = noc::TxnRecord::R_EJECT;
      r.msg_id = ex->msg_id; r.hops = ex->hops;
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
      return tlm::TLM_COMPLETED;    // 四相合法短路：终点没有下游
    }
    return tlm::TLM_ACCEPTED;
  }

  void b_fw(tlm::tlm_generic_payload& trans, sc_core::sc_time& d)
  {
    d += sc_core::sc_time(1, sc_core::SC_NS);
    noc::rv_ext* rv = trans.get_extension<noc::rv_ext>();
    if (rv && rv->valid) {
      rv->ready = true;                              // 终点永远可接收
      rv->handshake_at = sc_core::sc_time_stamp() + d;
    }
    record(trans, d);
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
  }

  unsigned int dbg_fw(tlm::tlm_generic_payload& trans)
  {
    unsigned char* p = trans.get_data_ptr();
    if (!p) return 0;
    const unsigned int n = std::min<unsigned int>(trans.get_data_length(),
                                                  (unsigned int)regs.size());
    for (unsigned int i = 0; i < n; ++i) p[i] = regs[i];
    return n;
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

SC_MODULE(NullSink)
{
  typedef noc::noc_protocol_types PT;
  tlm_utils::multi_passthrough_target_socket<NullSink, 64, PT> in;
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
  void b_fw(int, tlm::tlm_generic_payload&, sc_core::sc_time&) { ++n_dropped; }
};

// multi_passthrough_target_socket 至少要有 1 个绑定，否则 elaboration 会报
// "complete binding failed: port not bound"。torus / ring 的所有端口都有邻居，
// NullSink 可能一个都没绑上，因此用一个永不出事务的 initiator 兜底。
SC_MODULE(DummyInit)
{
  typedef noc::noc_protocol_types PT;
  tlm_utils::simple_initiator_socket<DummyInit, 64, PT> sock;
  explicit DummyInit(sc_core::sc_module_name nm)
    : sc_core::sc_module(nm), sock("sock") {}
};

// ---------------------------------------------------------------------------
// AT 注入端：nb_transport_fw + credit-based 流控（NI 持有 ni_window 个 credit）
// ---------------------------------------------------------------------------
SC_MODULE(TrafficGenTxn)
{
  typedef noc::noc_protocol_types PT;
  tlm_utils::simple_initiator_socket<TrafficGenTxn, 64, PT> sock;
  noc::txn_mm* mm;
  noc::RouterConfig cfg;
  int src = 0;
  int inj_credit = 0;
  tlm_utils::instance_specific_extension_accessor acc;

  SC_HAS_PROCESS(TrafficGenTxn);
  TrafficGenTxn(sc_core::sc_module_name nm, noc::txn_mm* m, noc::RouterConfig c, int s)
    : sc_core::sc_module(nm), sock("sock"), mm(m), cfg(c), src(s),
      inj_credit(c.ni_window)
  {
    sock.register_nb_transport_bw(this, &TrafficGenTxn::nb_bw);
    sock.register_invalidate_direct_mem_ptr(this, &TrafficGenTxn::invalidate_dmi);
    SC_THREAD(run);
  }

  void run()
  {
    for (int m = 0; m < cfg.n_messages; ++m) {
      const int dst = noc::pick_dst(cfg.traffic, src, m,
                                    cfg.mesh_x, cfg.mesh_y, src);
      // credit 用尽就等待归还：这是 AT 唯一的硬反压点，不丢消息
      while (inj_credit <= 0) wait(cfg.clk);
      tlm::tlm_generic_payload* t = make_txn(*mm, src, dst, m,
                                             cfg.msg_bytes, m + src);
      acc(*t).set_extension(new inj_ext(m));
      --inj_credit;
      t->acquire();
      tlm::tlm_phase ph(tlm::BEGIN_REQ);
      sc_core::sc_time d = sc_core::SC_ZERO_TIME;
      tlm::tlm_sync_enum s = sock->nb_transport_fw(*t, ph, d);
      if (s == tlm::TLM_COMPLETED) { ++inj_credit; t->release(); }
      wait(cfg.clk * cfg.inject_gap_cycles);
    }
  }

  tlm::tlm_sync_enum nb_bw(tlm::tlm_generic_payload& trans, tlm::tlm_phase& phase,
                           sc_core::sc_time&)
  {
    if (phase == tlm::BEGIN_RESP) {
      inj_ext* e = nullptr;
      acc(trans).get_extension(e);
      if (e) { ++inj_credit; acc(trans).clear_extension(e); }
      tlm::tlm_phase p(tlm::END_RESP);
      sc_core::sc_time z = sc_core::SC_ZERO_TIME;
      sock->nb_transport_fw(trans, p, z);
      trans.release();
      return tlm::TLM_COMPLETED;
    }
    return tlm::TLM_ACCEPTED;
  }
  void invalidate_dmi(sc_dt::uint64, sc_dt::uint64) {}
};

// ---------------------------------------------------------------------------
// LT 注入端：b_transport + temporal decoupling
// ---------------------------------------------------------------------------
SC_MODULE(TrafficGenLT)
{
  typedef noc::noc_protocol_types PT;
  tlm_utils::simple_initiator_socket<TrafficGenLT, 64, PT> sock;
  tlm_utils::tlm_quantumkeeper qk;
  noc::txn_mm* mm;
  noc::RouterConfig cfg;
  int src = 0;

  // ready/valid 握手统计（LT 无反压，stalls 恒 0）
  unsigned long n_valid = 0, n_ready = 0, n_stall = 0;
  unsigned long long bytes_offered = 0;

  SC_HAS_PROCESS(TrafficGenLT);
  TrafficGenLT(sc_core::sc_module_name nm, noc::txn_mm* m, noc::RouterConfig c, int s)
    : sc_core::sc_module(nm), sock("sock"), mm(m), cfg(c), src(s)
  {
    sock.register_invalidate_direct_mem_ptr(this, &TrafficGenLT::invalidate_dmi);
    SC_THREAD(run);
  }

  void run()
  {
    qk.reset();
    tlm_utils::tlm_quantumkeeper::set_global_quantum(sc_core::sc_time(100, sc_core::SC_NS));
    for (int m = 0; m < cfg.n_messages; ++m) {
      const int dst = noc::pick_dst(cfg.traffic, src, m,
                                    cfg.mesh_x, cfg.mesh_y, src);
      tlm::tlm_generic_payload* t = make_txn(*mm, src, dst, m,
                                             cfg.msg_bytes, m + src);
      // temporal decoupling：sc_time_stamp() 落后于本地时间，
      // 注入时刻必须是「同步基准 + 本地偏移」，延迟统计才不失真
      t->get_extension<noc::txn_ext>()->inject_time =
          sc_core::sc_time_stamp() + qk.get_local_time();
      t->acquire();
      sc_core::sc_time d = qk.get_local_time();
      sock->b_transport(*t, d);
      // ready/valid：b_transport 返回即本次事务沿整条路径走完；
      // 下游对每次 valid 都立即 ready，没有重试、没有信用计数。
      noc::rv_ext* rv = t->get_extension<noc::rv_ext>();
      if (rv) {
        if (rv->valid) ++n_valid;
        if (rv->ready) ++n_ready; else ++n_stall;
        bytes_offered += cfg.msg_bytes;
      }
      t->release();
      d += cfg.clk * cfg.inject_gap_cycles;
      qk.set(d);
      if (qk.need_sync()) qk.sync();
    }
    qk.sync();
  }
  void invalidate_dmi(sc_dt::uint64, sc_dt::uint64) {}
};

// ---------------------------------------------------------------------------
// 统一连线：同一个 router 类拼出 mesh / torus / ring
// ---------------------------------------------------------------------------
template <typename R>
static void bind_router(R* r, int id, int p, const std::vector<R*>& all,
                        NetworkSink* sink, NullSink* null_sink,
                        const noc::RouterConfig& cfg)
{
  if (p == noc::P_L) { r->out[p]->bind(sink->sock); return; }
  const int x = id % cfg.mesh_x, y = id / cfg.mesh_x;
  const int nid = noc::neighbor(cfg.topology, x, y, p, cfg.mesh_x, cfg.mesh_y);
  if (nid >= 0) r->out[p]->bind(all[nid]->in);
  else          r->out[p]->bind(null_sink->in);
}

// ---------------------------------------------------------------------------
int sc_main(int argc, char* argv[])
{
  const std::string mode    = (argc > 1)  ? argv[1]  : "at";
  noc::RouterConfig base;
  base.mesh_x       = (argc > 2)  ? std::atoi(argv[2])  : 4;
  base.mesh_y       = (argc > 3)  ? std::atoi(argv[3])  : 4;
  base.n_messages   = (argc > 4)  ? std::atoi(argv[4])  : 30;
  base.inject_gap_cycles = (argc > 5) ? std::atoi(argv[5]) : 8;
  base.routing      = noc::parse_routing (argc > 6  ? argv[6]  : "xy");
  base.topology     = noc::parse_topology(argc > 7  ? argv[7]  : "mesh");
  base.traffic      = noc::parse_traffic (argc > 8  ? argv[8]  : "uniform");
  base.msg_bytes    = (argc > 9)  ? std::atoi(argv[9])  : 64;
  base.link_bw      = (argc > 10) ? std::atof(argv[10]) : 16.0;
  if (base.link_bw <= 0.0) base.link_bw = 16.0;
  if (base.topology == noc::TOPO_RING) base.mesh_y = 1;
  const bool use_lt = (mode == "lt");
  const int n_nodes = base.mesh_x * base.mesh_y;

  noc::txn_mm mm;
  NullSink null_sink("null_sink");
  DummyInit dummy("dummy");
  dummy.sock.bind(null_sink.in);
  PerfMonitor monitor("monitor");

  std::vector<NetworkSink*> sinks(n_nodes);
  std::vector<RouterTxn*>   at(n_nodes, nullptr);
  std::vector<RouterTxnLT*> lt(n_nodes, nullptr);
  std::vector<TrafficGenTxn*> gen_at(n_nodes, nullptr);
  std::vector<TrafficGenLT*>  gen_lt(n_nodes, nullptr);

  for (int i = 0; i < n_nodes; ++i) {
    noc::RouterConfig c = base;
    c.id = i; noc::id2xy(i, c.mesh_x, c.x, c.y);
    sinks[i] = new NetworkSink((std::string("sink_") + std::to_string(i)).c_str());
    sinks[i]->stat.bind(monitor);
    if (use_lt) {
      lt[i] = new RouterTxnLT((std::string("router_") + std::to_string(i)).c_str(), c);
      lt[i]->stat_port.bind(monitor);
      gen_lt[i] = new TrafficGenLT((std::string("gen_") + std::to_string(i)).c_str(), &mm, c, i);
      gen_lt[i]->sock.bind(lt[i]->in);
    } else {
      at[i] = new RouterTxn((std::string("router_") + std::to_string(i)).c_str(), c);
      at[i]->stat_port.bind(monitor);
      gen_at[i] = new TrafficGenTxn((std::string("gen_") + std::to_string(i)).c_str(), &mm, c, i);
      gen_at[i]->sock.bind(at[i]->in);
    }
  }
  for (int i = 0; i < n_nodes; ++i)
    for (int p = 0; p < noc::N_PORTS; ++p) {
      if (use_lt) bind_router(lt[i], i, p, lt, sinks[i], &null_sink, base);
      else        bind_router(at[i], i, p, at, sinks[i], &null_sink, base);
    }

  // DMI / debug transport 演示：无副作用、不推进时间
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
    sinks[0]->sock->invalidate_direct_mem_ptr(0, 255);
  }

  std::cout << "=== NoC " << base.mesh_x << "x" << base.mesh_y
            << " | " << noc::topo_name(base.topology)
            << " | " << noc::routing_name(base.routing)
            << " | " << noc::traffic_name(base.traffic)
            << " | style=" << mode
            << " | msgs/node=" << base.n_messages
            << " | gap=" << base.inject_gap_cycles << " cycles ===\n";

  // 只量 sc_start 本身，排除进程启动 / elaboration 开销
  const auto wall_t0 = std::chrono::steady_clock::now();
  sc_core::sc_start(sc_core::sc_time(200, sc_core::SC_US));
  const auto wall_t1 = std::chrono::steady_clock::now();
  const double wall_ms =
      std::chrono::duration<double, std::milli>(wall_t1 - wall_t0).count();

  const double span = monitor.span_ns();
  const double tput = (span > 0.0) ? monitor.n_eject / (span / 1000.0) : 0.0;

  double max_link_util = 0.0;
  unsigned long n_queued = 0, n_forwarded = 0;
  unsigned long credit_issued = 0, credit_returned = 0;
  unsigned long rv_valid = 0, rv_ready = 0, rv_stall = 0, rv_hops = 0;
  double sum_wait = 0.0;
  int max_occ = 0;
  if (!use_lt) {
    for (int i = 0; i < n_nodes; ++i) {
      n_queued    += at[i]->n_queued;
      n_forwarded += at[i]->n_forwarded;
      sum_wait    += at[i]->sum_wait_ns;
      credit_issued   += at[i]->n_credit_issued;
      credit_returned += at[i]->n_credit_returned;
      for (int p = 0; p < noc::N_PORTS; ++p) {
        max_link_util = std::max(max_link_util, at[i]->link_utilization(p));
        max_occ = std::max(max_occ, at[i]->max_occupancy[p]);
      }
    }
  } else {
    for (int i = 0; i < n_nodes; ++i) {
      rv_valid += gen_lt[i]->n_valid;
      rv_ready += gen_lt[i]->n_ready;
      rv_stall += gen_lt[i]->n_stall;
      rv_hops  += static_cast<unsigned long>(lt[i]->n_handshake);
    }
  }

  std::cout << std::fixed << std::setprecision(4);
  std::cout << "  ejected messages = " << monitor.n_eject
            << ", forwarded hops = " << monitor.n_hop << "\n";
  std::cout << "  avg latency = " << monitor.avg_latency()
            << " ns, p95 = " << monitor.p95_latency()
            << " ns, max = " << monitor.max_latency()
            << " ns, avg hops = "
            << (monitor.n_eject ? (double)monitor.sum_hops / monitor.n_eject : 0.0) << "\n";
  std::cout << "  throughput = " << tput << " msgs/us"
            << ", observation span = " << span << " ns\n";
  if (!use_lt) {
    std::cout << "  [AT] queued hops = " << n_queued << "/" << n_forwarded
              << ", avg queue wait = "
              << (n_queued ? sum_wait / n_queued : 0.0)
              << " ns, p95 = " << monitor.p95_wait()
              << " ns, max = " << monitor.max_wait() << " ns\n";
    std::cout << "  [AT] max link utilization = " << max_link_util
              << ", peak input queue depth = " << max_occ
              << " msgs (NI credit window = " << base.ni_window << ")\n";
    std::cout << "  [AT] credit: issued = " << credit_issued
              << ", returned = " << credit_returned
              << ", in flight at end = "
              << (credit_issued - credit_returned) << "\n";
  } else {
    const double t_ser = base.t_serialize(base.msg_bytes).to_seconds() * 1e9;
    const double t_svc = base.t_service(base.msg_bytes).to_seconds() * 1e9;
    const double inj_rate =
        1.0 / (base.inject_gap_cycles * base.clk.to_seconds() * 1e9);
    std::cout << "  [LT] ready/valid: valid = " << rv_valid
              << ", ready = " << rv_ready << ", stalls = " << rv_stall
              << ", router handshakes = " << rv_hops << "\n";
    std::cout << "  [LT] bandwidth check: t_serialize = " << t_ser
              << " ns, t_service = " << t_svc
              << " ns, link service rate = " << (1.0 / t_svc)
              << " msgs/ns, per-node injection = " << inj_rate << " msgs/ns\n";
  }
  std::cout << "  dropped at boundary = " << null_sink.n_dropped
            << ", sc_start wall time = " << wall_ms << " ms\n";

  // 参数扫描脚本读取这一行
  std::cout << "CSV: " << mode << "," << noc::topo_name(base.topology) << ","
            << noc::routing_name(base.routing) << ","
            << noc::traffic_name(base.traffic) << ","
            << base.inject_gap_cycles << "," << base.msg_bytes << ","
            << base.link_bw << ","
            << monitor.avg_latency() << "," << monitor.p95_latency() << ","
            << monitor.max_latency() << "," << tput << ","
            << (n_queued ? sum_wait / n_queued : 0.0) << ","
            << monitor.max_wait() << ","
            << (n_forwarded ? (double)n_queued / n_forwarded : 0.0) << ","
            << max_link_util << ","
            << (monitor.n_eject ? (double)monitor.sum_hops / monitor.n_eject : 0.0) << ","
            << monitor.n_eject << "," << null_sink.n_dropped << ","
            << wall_ms << "\n";
  std::cout << "=== done ===\n";
  return 0;
}
