# SystemC TLM Router：transaction 级架构探索模型

一个可编译、可运行、可复现的 SystemC TLM-2.0 示例仓库。以 NoC router 为例讲清楚「怎么用 TLM 做架构探索」：**只建一个统一 router 模型**，换拓扑、路由、流量、消息长度、链路带宽都不需要改 router 代码。

- 文章：[SystemC-TLM-Router建模.md](SystemC-TLM-Router建模.md) ｜ [网页阅读版](SystemC-TLM-Router建模.html)
- 真实 spec：Mullins / West / Moore, ASPDAC 2006, Fig.1，从论文 PDF 裁出，见 `assets/router-arch-spec.png`；流水线与参数体系参照 BookSim2 `IQRouter` 的 `RC → VA → SA → ST`

## 一句话结论

| | **LT router** | **AT router** |
| --- | --- | --- |
| 流控协议 | ready / valid 握手 | credit 流控 |
| 时间模型 | `b_transport` 累加每跳固定服务时间 | `nb_transport` 四相 + `sc_time&` 时间注解 + 链路排队 |
| 探索粒度 | **粗粒度：连通性和带宽够不够** | **细粒度：延迟和拥塞怎么样** |
| 能看见 | 目的可达、平均跳数、链路服务率、需求带宽 | 延迟-负载曲线、饱和吞吐、排队等待、最忙链路利用率 |

去掉逐周期细节可以，**丢掉竞争不行**。AT 把拥塞上移到「输出链路 = 单服务器」：

```
arrival = now + delay
start   = max(arrival + t_rc, link_free_at[op])
service = t_link + msg_bytes / link_bw
delay   = start + service - now
```

## 快速开始

### 依赖

| 依赖 | 说明 |
| --- | --- |
| SystemC 2.3.4 | 需从源码编译安装，**库与示例必须用同一个 C++ 标准**（本文用 C++14），否则链接时报 `sc_api_version_2_3_4_cxx201402L` 未定义 |
| g++ / MinGW-w64 | 支持 `-std=c++14` |
| Python 3 | 仅标准库，用于参数扫描、出图、同步文章、生成 HTML |

`SYSTEMC` 指向含 `include/` 与 `lib/` 的安装前缀。

### 编译

```bash
cd code
make SYSTEMC=/path/to/systemc          # 生成 noc_txn_demo 与 tour
make run-tour                           # TLM 接口特性巡礼
```

默认值 `SYSTEMC ?= ../_tmp/systemc-c14` 对应本仓库作者在 Windows 上的本地构建目录；Linux / macOS 请显式传入自己的安装前缀。MSYS make 在 Windows 下会把子进程临时目录指向不可写的 `C:\WINDOWS\`，两个 Makefile 已自动处理。

### 运行

```bash
cd code
./noc_txn_demo <at|lt> <mx> <my> <msgs/node> <gap_cycles> \
               <xy|yx|oblivious> <mesh|torus|ring> \
               <uniform|transpose|bitcomp|hotspot> <msg_bytes> <link_bw_B/ns>

./noc_txn_demo at 4 4 30 8 xy mesh uniform 64 16    # AT：credit 流控
./noc_txn_demo lt 4 4 30 8 xy mesh uniform 64 16    # LT：ready/valid

make run-at GAP=8
make run-lt GAP=8
```

每行输出末尾带一条机器可解析的 `CSV:`，字段见 `tools/sweep.py` 的 `COLS`。AT 模式额外输出 credit 守恒统计，LT 模式额外输出 ready/valid 握手与带宽账。

### 一键复现全部实验与文章数字

```bash
cd code && make sweep
```

按顺序执行四步，全部结果可复现：

1. `tools/sweep.py` —— 43 轮参数扫描，写 `data/dse_results.csv` 与 `data/dse_meta.json`
2. `tools/make_figs_txn.py` —— 由数据生成 10 张 SVG 配图到 `assets/`
3. `tools/refresh_article.py` —— 同步文章中的全部实测表格与正文数字，每个锚点必须命中，并自检正文一致性
4. `tools/md2html.py` —— 生成网页阅读版

## 目录结构

```
SystemC-TLM-Router建模.md      文章正文（15 节，含 TLM 基础、router 原理、两套协议、实验与坑）
SystemC-TLM-Router建模.html    网页阅读版
assets/                       10 张主线配图 + 真实论文 Fig.1（PNG）
data/dse_results.csv          43 轮扫描原始结果
data/dse_meta.json            计时元数据
tools/                        扫描 / 出图 / 同步 / HTML 四个流水线脚本
code/
  noc_txn_common.h            公共类型：txn_ext / rv_ext / HopCtx、扩展相位、路由与流量、配置
  router_txn.h                AT router：credit、链路服务模型、四相、PEQ、DMI/debug、统计
  router_txn_lt.h             LT router：ready/valid + b_transport 固定延迟累加
  noc_txn_top.cpp             统一顶层：三种拓扑、三种路由、四种流量、两套注入器、CSV 输出
  tlm_feature_tour.cpp        TLM-2.0 接口特性巡礼
  Makefile                    编译 / 运行 / 扫描
  flit_level/                 早期周期级对照模型源码（可单独编译，文章主线不再引用）
assets/flit_level/            早期对照配图（文章主线不再引用）
```

## 实验矩阵与关键结论

| 实验 | 扫描范围 | 结论 |
| --- | --- | --- |
| 注入率 | gap 64 → 1 | AT 延迟从约 24 ns 涨到约 72 ns 并出现饱和吞吐；LT 恒为 23 ns，看不到饱和点 |
| 拓扑 | mesh / torus / ring | torus 平均跳数更低，ring 因跳数高显著劣化 |
| 路由 × 流量 | XY / YX / oblivious × 4 种流量 | oblivious 改善 transpose / bitcomp，但在 uniform / hotspot 下变差，没有放之四海皆准的最优解 |
| 消息长度 / 带宽 | 32–128 B × 16–64 B/ns | `msg_bytes / link_bw` 是唯一的服务需求比值，等比缩放得到相同延迟 |

具体数字以 `data/dse_results.csv` 与文章 §11 为准；计时类数字受系统调度影响会有波动，正文一律引用量级。

## 能力边界

**能回答**：拓扑 / 路由 / 容量 / 消息粒度 / 带宽的趋势排序、延迟-负载曲线、饱和点、链路利用率。

**不能回答**：VC 数量与分配策略、arbiter 微架构、wormhole HOL blocking、cycle 级尾延迟、精确 power / area、协议级死锁的形式化证明。这些问题需要周期级模型或 RTL / BookSim2 复核。文章 §12 给出「快模型搜索、慢模型验证」的混合策略。

## TLM-2.0 特性覆盖

generic payload 与 `tlm_extension` / instance-specific extension、自定义 protocol types、标准与扩展 phase、`simple_initiator/target_socket`、`nb_transport_fw/bw` 四相、`b_transport`、DMI、`transport_dbg`、PEQ（`peq_with_cb_and_phase` / `peq_with_get`）、memory manager、`tlm_quantumkeeper` temporal decoupling、`tlm_global_quantum`、`analysis_port`、TLM_COMPLETED 早完成、大小端转换、`TLM_OK_RESPONSE`。

## 参考

1. R. Mullins, A. West, S. Moore, *The Design and Implementation of a Low-Latency On-Chip Network*, ASPDAC 2006 —— Fig.1 架构图来源。
2. N. Jiang et al., *A Detailed and Flexible Cycle-Accurate Network-on-Chip Simulator* (BookSim2), ISPASS 2013 —— 四级流水与 evaluate/update 协议。
3. *BookSim 2.0 User's Guide*, §4.5.1 / §4.6 —— 参数体系。

完整链接见文章末尾「参考」一节。
