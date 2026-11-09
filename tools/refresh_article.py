# -*- coding: utf-8 -*-
"""从 dse_results.csv / dse_meta.json 同步文章里的全部实测数字与表格。

正文用「锚点正则」同步，表格用整块覆盖：
  * 不依赖任何硬编码的历史数值，改模型后重新 make sweep 也能对齐；
  * 每个锚点必须命中指定次数，文章结构变了会立刻报错而不是静默漏改；
  * 自检保证正文不出现 flit、不残留归档模型对比。
"""
import os
import csv
import io
import json
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MD = os.path.join(ROOT, "SystemC-TLM-Router建模.md")
CSV = os.path.join(ROOT, "data", "dse_results.csv")
META = os.path.join(ROOT, "data", "dse_meta.json")

with io.open(CSV, encoding="utf-8") as f:
    R = list(csv.DictReader(f))
with io.open(META, encoding="utf-8") as f:
    M = json.load(f)


def sel(sec, **kw):
    o = [r for r in R if r["section"] == sec]
    for k, v in kw.items():
        o = [r for r in o if str(r[k]) == str(v)]
    return o


def v(r, k):
    return float(r[k])


def f2(x):
    return "%.2f" % x


def f1(x):
    return "%.1f" % x


def f0(x):
    return "%.0f" % x


RA = sorted(sel("rate", mode="at"), key=lambda r: -int(r["gap"]))
RL = {int(r["gap"]): r for r in sel("rate", mode="lt")}
T4 = {r["topo"]: r for r in sel("topology") if r["gap"] == "4"}
T8 = {r["topo"]: r for r in sel("topology") if r["gap"] == "8"}
RO = {(r["routing"], r["traffic"]): r for r in sel("routing")}
MS = {r["msg_bytes"]: r for r in sel("msglen")}
BW = {int(float(r["link_bw"])): r for r in sel("bandwidth")}

LAT0, LAT1 = v(RA[0], "avg_lat_ns"), v(RA[-1], "avg_lat_ns")
LTLAT = v(RL[int(RA[0]["gap"])], "avg_lat_ns")
SAT_TPUT = round(v(T4["mesh"], "tput_msg_per_us") / 100.0) * 100


def replace_table(md, header, body):
    lines = md.split("\n")
    idx = next(i for i, l in enumerate(lines) if l.startswith(header))
    end = idx
    while end < len(lines) and lines[end].startswith("|"):
        end += 1
    new = body.rstrip("\n").split("\n")
    return "\n".join(lines[:idx] + new + lines[end:])


# ---- §11.1 注入率扫描 ----
t = ["| gap (cycles) | AT 平均延迟 | AT 吞吐 (msgs/us) | 最忙链路利用率 | LT 平均延迟 |",
     "| --- | --- | --- | --- | --- |"]
for r in RA:
    g = int(r["gap"])
    t.append("| %d | %s ns | %s | %.3f | %s ns |"
             % (g, f2(v(r, "avg_lat_ns")), f2(v(r, "tput_msg_per_us")),
                v(r, "max_link_util"), f2(v(RL[g], "avg_lat_ns"))))
T_RATE = "\n".join(t)

# ---- §11.2 拓扑 ----
t = ["| 拓扑 | 平均跳数 | gap=8 延迟 | gap=4 延迟 | gap=4 吞吐 |",
     "| --- | --- | --- | --- | --- |"]
for key, lab in (("mesh", "4×4 mesh"), ("torus", "4×4 torus"), ("ring", "16 节点 ring")):
    r4, r8 = T4[key], T8[key]
    bold = "**%s**" % f2(v(r4, "avg_lat_ns")) if key == "torus" else f2(v(r4, "avg_lat_ns"))
    hops = "**%s**" % r4["avg_hops"] if key == "torus" else r4["avg_hops"]
    t.append("| %s | %s | %s ns | %s ns | %s msgs/us |"
             % (lab, hops, f2(v(r8, "avg_lat_ns")), bold,
                f0(v(r4, "tput_msg_per_us"))))
T_TOPO = "\n".join(t)

# ---- §11.3 路由 × 流量 ----
t = ["| 流量 | XY | YX | Oblivious |", "| --- | --- | --- | --- |"]
for tr, lab in (("uniform", "uniform"), ("transpose", "transpose"),
                ("bitcomp", "bitcomp"), ("hotspot", "hotspot")):
    vals = {k: v(RO[(k, tr)], "avg_lat_ns") for k in ("xy", "yx", "oblivious")}
    best = min(vals, key=vals.get)
    cells = ["**%s**" % f2(vals[k]) if k == best else f2(vals[k])
             for k in ("xy", "yx", "oblivious")]
    t.append("| %s | %s |" % (lab, " | ".join(cells)))
T_ROUT = "\n".join(t)

# ---- §11.4 消息长度 / 带宽 ----
t = ["| 消息长度（bw=16） | 平均延迟 | 吞吐 | 链路带宽（msg=64 B） | 平均延迟 | 吞吐 |",
     "| --- | --- | --- | --- | --- | --- |"]
ml = [(int(k), MS[k]) for k in sorted(MS, key=lambda x: int(x))]
bw = [(k, BW[k]) for k in sorted(BW)]
for i in range(max(len(ml), len(bw))):
    left = ("%d B | %s ns | %s msgs/us" % (ml[i][0], f2(v(ml[i][1], "avg_lat_ns")),
                                           f0(v(ml[i][1], "tput_msg_per_us")))) \
        if i < len(ml) else "— | — | —"
    right = ("%d B/ns | %s ns | %s msgs/us" % (bw[i][0], f2(v(bw[i][1], "avg_lat_ns")),
                                               f0(v(bw[i][1], "tput_msg_per_us")))) \
        if i < len(bw) else "— | — | —"
    t.append("| %s | %s |" % (left, right))
T_MSGBW = "\n".join(t)

# ---- §11.6 速度 ----
T_SPEED = "\n".join([
    "| 对比 | 实测 | 说明 |",
    "| --- | --- | --- |",
    "| AT vs LT（`sc_start` 内核） | %.2f ms vs %.2f ms，**慢 %.1f×** | 9 次取最小，排除进程启动 |"
    % (M["kernel_at_ms"], M["kernel_lt_ms"], M["kernel_ratio"]),
    "| %d 轮扫描主体 | %.1f s | 含 %d 次进程启动、CSV 汇总与计时基准 |"
    % (M["n_runs"], M["sweep_total_s"], M["n_runs"]),
])

with io.open(MD, encoding="utf-8") as f:
    md = f.read()


def sync(pattern, repl, expected=1):
    """按上下文锚点替换正文数字；命中次数不符立即报错。"""
    global md
    md, n = re.subn(pattern, repl, md, flags=re.M)
    if n != expected:
        raise SystemExit("refresh_article: 锚点 %r 命中 %d 次（期望 %d）"
                         % (pattern, n, expected))


NUM = r'[\d.]+'

# ---- 正文数字：全部由 CSV 派生，且锚点驱动（不依赖旧值） ----
sync(r'（AT ' + NUM + r' ns / LT ' + NUM + r' ns）',
     '（AT %s ns / LT %s ns）' % (f2(LAT0), f2(LTLAT)))
sync(r'AT 的延迟随负载从 ' + NUM + r' 涨到 ' + NUM + r' ns 而 LT 恒为 ' + NUM + r' ns',
     'AT 的延迟随负载从 %s 涨到 %s ns 而 LT 恒为 %s ns'
     % (f2(LAT0), f2(LAT1), f2(LTLAT)))

# §4.3 轻重载对照表
sync(r'\| LT（ready/valid） \| ' + NUM + r' ns \| ' + NUM + r' ns \|',
     '| LT（ready/valid） | %s ns | %s ns |' % (f2(LTLAT), f2(LTLAT)))
sync(r'\| AT（credit \+ 竞争） \| ' + NUM + r' ns \| ' + NUM + r' ns \|',
     '| AT（credit + 竞争） | %s ns | %s ns |' % (f2(LAT0), f2(LAT1)))

# §5.2 解析锚点表
sync(r'\| AT ' + NUM + r' / LT ' + NUM + r' ns \|',
     '| AT %s / LT %s ns |' % (f2(LAT0), f2(LTLAT)))
sync(r'全系统饱和于 ≈' + NUM + r' msgs/us',
     '全系统饱和于 ≈%d msgs/us' % SAT_TPUT)
sync(r'吞吐在 ≈' + NUM + r' msgs/us 封顶',
     '吞吐在 ≈%d msgs/us 封顶' % SAT_TPUT)
sync(r'LT 一列从头到尾 ' + NUM + r' ns',
     'LT 一列从头到尾 %s ns' % f2(LTLAT))

# §11.2 拓扑正文
sync(r'平均跳数从 ' + NUM + r' 降到 ' + NUM,
     '平均跳数从 %s 降到 %s'
     % (f2(v(T4["mesh"], "avg_hops")), f2(v(T4["torus"], "avg_hops"))))
_ring_ratio = max(1, int(round(v(T4["mesh"], "tput_msg_per_us")
                              / max(v(T4["ring"], "tput_msg_per_us"), 1.0))))
sync(r'ring 平均跳数 ' + NUM + r'，吞吐只有 mesh 的 1/\d+',
     'ring 平均跳数 %s，吞吐只有 mesh 的 1/%d'
     % (f2(v(T4["ring"], "avg_hops")), _ring_ratio))

# §11.3 路由正文
sync(r'从 ' + NUM + r' / ' + NUM + r' ns 降到 ' + NUM + r' ns',
     '从 %s / %s ns 降到 %s ns'
     % (f1(v(RO[("xy", "transpose")], "avg_lat_ns")),
        f1(v(RO[("xy", "bitcomp")], "avg_lat_ns")),
        f1(v(RO[("oblivious", "transpose")], "avg_lat_ns"))))
sync(r'oblivious 在均匀流量下从 ' + NUM + r' 涨到 ' + NUM
     + r' ns，在热点流量下从 ' + NUM + r' 涨到 ' + NUM + r' ns',
     'oblivious 在均匀流量下从 %s 涨到 %s ns，在热点流量下从 %s 涨到 %s ns'
     % (f1(v(RO[("xy", "uniform")], "avg_lat_ns")),
        f1(v(RO[("oblivious", "uniform")], "avg_lat_ns")),
        f1(v(RO[("xy", "hotspot")], "avg_lat_ns")),
        f1(v(RO[("oblivious", "hotspot")], "avg_lat_ns"))))

# §11.4 服务模型自洽性
sync(r'完全相同的 ' + NUM + r' ns',
     '完全相同的 %s ns' % f2(v(MS["32"], "avg_lat_ns")))

# §11.5 LT 带宽账
sync(r'`avg_hops = ' + NUM + r'`',
     '`avg_hops = %s`' % T4["mesh"]["avg_hops"])
sync(r'\+ sink = ' + NUM + r' ns',
     '+ sink = %s ns' % f2(LTLAT))

# ---- 扫描总耗时：毫秒/秒级抖动大，正文只写量级，表格保留当次实测 ----
sync(r'43 轮参数扫描主体(?:十余秒|在数秒内完成|' + NUM + r'\s*s)',
     '43 轮参数扫描主体在数秒内完成')
sync(r'43 轮扫描主体(?:十余秒|在数秒内完成|' + NUM + r'\s*s)',
     '43 轮扫描主体在数秒内完成')

# ---- 计时说明：由流水线幂等写入 ----
NOTE = ("> 计时说明：毫秒级测量受系统调度影响，重复运行上表倍率会明显波动"
        "（历史实测 5–9×）；正文引用的是量级，不是精确值。")
sync(r'^> 计时说明：.*$', NOTE, 1)

# ---- 自检 ----
problems = []
if re.search(r'flit', md, re.I):
    problems.append("文章中出现 flit")
if re.search(r'\| 本文实测耗时 \|', md):
    problems.append("层级对照表仍含归档模型耗时行")
if re.search(r'实测快|归档的\s*flit|回到\s*flit', md):
    problems.append("仍含归档模型速度对比")
if re.search(r'40–50×|7–9×|±20%|49\.3×|46 毫秒', md):
    problems.append("仍含旧版速度表述")
if problems:
    raise SystemExit("refresh_article: " + "；".join(problems))

# 表格最后覆盖，保证单元格是纯净的、来自 CSV 的值
md = replace_table(md, "| gap (cycles) | AT 平均延迟", T_RATE)
md = replace_table(md, "| 拓扑 | 平均跳数 |", T_TOPO)
md = replace_table(md, "| 流量 | XY | YX |", T_ROUT)
md = replace_table(md, "| 消息长度（bw=16） |", T_MSGBW)
md = replace_table(md, "| 对比 | 实测 |", T_SPEED)

with io.open(MD, "w", encoding="utf-8") as f:
    f.write(md)
print("synced", MD)
print("lat0=%.2f lat1=%.2f lt=%.2f sat=%d kernel=%.1fx sweep=%.1fs"
      % (LAT0, LAT1, LTLAT, SAT_TPUT, M["kernel_ratio"], M["sweep_total_s"]))
