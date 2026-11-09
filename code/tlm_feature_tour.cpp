// ============================================================================
// tlm_feature_tour.cpp -- 核心 router 模型里没用到的 TLM-2.0 接口补齐演示
// 单独编译：g++ -std=c++14 -I$SYSTEMC/include tlm_feature_tour.cpp -lsystemc
// 只演示 API 语义，不参与 NoC 仿真。
// ============================================================================
#include <systemc>
#include <tlm>
#include <tlm_utils/peq_with_get.h>
#include <tlm_utils/peq_with_cb_and_phase.h>
#include <tlm_utils/multi_passthrough_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/tlm_quantumkeeper.h>
#include <tlm_core/tlm_2/tlm_generic_payload/tlm_endian_conv.h>

#include <iostream>
#include "noc_txn_common.h"

// ---------------------------------------------------------------------------
// A) peq_with_get：另一种 PEQ（自己 wait 事件、自己 get），以及 cancel_all()
// ---------------------------------------------------------------------------
SC_MODULE(PeqGetDemo)
{
  tlm_utils::peq_with_get<tlm::tlm_generic_payload> peq;
  SC_HAS_PROCESS(PeqGetDemo);
  explicit PeqGetDemo(sc_core::sc_module_name n) : sc_core::sc_module(n), peq("peq")
  { SC_THREAD(run); }

  void run()
  {
    tlm::tlm_generic_payload a, b;
    peq.notify(a, sc_core::sc_time(3, sc_core::SC_NS));
    peq.notify(b, sc_core::sc_time(7, sc_core::SC_NS));
    peq.cancel_all();                       // 复位模型时清空所有在途事件
    peq.notify(a, sc_core::sc_time(1, sc_core::SC_NS));
    wait(peq.get_event());
    unsigned int n = 0;
    while (tlm::tlm_generic_payload* t = peq.get_next_transaction()) { ++n; (void)t; }
    std::cout << "[A] peq_with_get: " << n << " transaction(s) after cancel_all\n";
  }
};

// ---------------------------------------------------------------------------
// B) multi_passthrough_initiator_socket：一个 socket 接多个 target
//    （和 AT router 里"一个 multi target 接多个 initiator"正好互补）
// ---------------------------------------------------------------------------
SC_MODULE(SimpleMem)
{
  typedef noc::noc_protocol_types PT;
  tlm_utils::simple_target_socket<SimpleMem, 32, PT> sock;
  unsigned char data[16] = {0};
  explicit SimpleMem(sc_core::sc_module_name n) : sc_core::sc_module(n), sock("sock")
  { sock.register_b_transport(this, &SimpleMem::b_transport); }
  void b_transport(tlm::tlm_generic_payload& t, sc_core::sc_time& d)
  {
    d += sc_core::sc_time(2, sc_core::SC_NS);
    if (t.is_read() && t.get_data_length() <= 16)
      for (unsigned int i = 0; i < t.get_data_length(); ++i) t.get_data_ptr()[i] = data[i];
    t.set_dmi_allowed(true);               // DMI hint
    t.set_response_status(tlm::TLM_OK_RESPONSE);
  }
};

SC_MODULE(MultiInitDemo)
{
  typedef noc::noc_protocol_types PT;
  tlm_utils::multi_passthrough_initiator_socket<MultiInitDemo, 32, PT> out;
  SC_HAS_PROCESS(MultiInitDemo);
  explicit MultiInitDemo(sc_core::sc_module_name n) : sc_core::sc_module(n), out("out")
  { SC_THREAD(run); }
  void run()
  {
    unsigned char buf[4] = {0};
    tlm::tlm_generic_payload t;
    t.set_command(tlm::TLM_READ_COMMAND);
    t.set_address(0);
    t.set_data_ptr(buf);
    t.set_data_length(4);
    t.set_streaming_width(4);
    sc_core::sc_time d = sc_core::SC_ZERO_TIME;
    out[0]->b_transport(t, d);             // [0] = 第 0 个绑定的 target
    std::cout << "[B] multi_passthrough_initiator_socket: bound=" << out.size()
              << ", annotated delay=" << d << "\n";
  }
};

// ---------------------------------------------------------------------------
// C) deep_copy_from / update_original_from / free_all_extensions / endianness
//    桥接两种协议时会用到：把 incoming transaction 复制一份再挂新扩展
// ---------------------------------------------------------------------------
static void payload_copy_demo()
{
  unsigned char src[4] = {0x11, 0x22, 0x33, 0x44};
  unsigned char dst[4] = {0, 0, 0, 0};
  noc::txn_mm mm;                              // auto extension 要求 payload 有 mm
  tlm::tlm_generic_payload orig, copy;
  orig.set_mm(&mm); copy.set_mm(&mm);
  orig.set_command(tlm::TLM_WRITE_COMMAND);
  orig.set_address(0x1000);
  orig.set_data_ptr(src);
  orig.set_data_length(4);
  orig.set_streaming_width(4);
  orig.set_auto_extension(new noc::txn_ext);    // 扩展会随 deep_copy 一起复制

  copy.set_data_ptr(dst);
  copy.set_data_length(4);
  copy.deep_copy_from(orig);
  copy.set_response_status(tlm::TLM_OK_RESPONSE);
  orig.update_original_from(copy);              // 把响应状态/扩展拷回原事务
  std::cout << "[C] deep_copy_from: byte0=" << std::hex << (int)dst[0]
            << ", response=" << std::dec << orig.get_response_string()
            << ", ref_count=" << orig.get_ref_count() << "\n";
  copy.free_all_extensions();                   // 克隆出来的事务要自己清理扩展

  // 大小端转换助手（总线宽度与 data 宽度不一致时）；DATAWORD 需显式指定
  tlm::tlm_to_hostendian_generic<unsigned int>(&orig, 4);
  tlm::tlm_from_hostendian_generic<unsigned int>(&orig, 4);
}

// ---------------------------------------------------------------------------
// D) tlm_analysis_port / tlm_dmi / global quantum
//    注：tlm_analysis_fifo 在部分 2.3.x + C++14 组合下有名字查找缺陷，
//    生产代码更常用 analysis_port + 自定义 tlm_analysis_if（见 noc_top.cpp）。
// ---------------------------------------------------------------------------
SC_MODULE(AnalysisDemo)
{
  tlm::tlm_analysis_port<int> port;
  struct Recv : tlm::tlm_analysis_if<int> {
    int v = 0;
    void write(const int& t) override { v = t; }
  } recv;
  SC_HAS_PROCESS(AnalysisDemo);
  explicit AnalysisDemo(sc_core::sc_module_name n)
    : sc_core::sc_module(n), port("port")
  { port.bind(recv); SC_THREAD(run); }
  void run()
  {
    port.write(42);
    std::cout << "[D] tlm_analysis_port: received " << recv.v << "\n";
  }
};

static void misc_demo()
{
  tlm::tlm_dmi dmi;
  unsigned char mem[8] = {0};
  dmi.set_dmi_ptr(mem);
  dmi.set_start_address(0x0);
  dmi.set_end_address(0x7);
  dmi.set_read_latency(sc_core::sc_time(1, sc_core::SC_NS));
  dmi.allow_read_write();
  std::cout << "[D] tlm_dmi: range 0x" << std::hex << dmi.get_start_address()
            << "-0x" << dmi.get_end_address() << std::dec
            << ", read_allowed=" << dmi.is_read_allowed() << "\n";

  tlm_utils::tlm_quantumkeeper::set_global_quantum(sc_core::sc_time(100, sc_core::SC_NS));
  std::cout << "[D] global quantum = "
            << tlm_utils::tlm_quantumkeeper::get_global_quantum() << "\n";
}

int sc_main(int argc, char* argv[])
{
  PeqGetDemo peq_demo("peq_demo");
  SimpleMem mem("mem");
  MultiInitDemo multi("multi");
  AnalysisDemo analysis("analysis");
  multi.out.bind(mem.sock);
  payload_copy_demo();
  misc_demo();
  sc_core::sc_start(sc_core::sc_time(10, sc_core::SC_NS));
  std::cout << "[tour] done\n";
  return 0;
}
