# -*- coding: utf-8 -*-
"""由 dse_results.csv 生成架构探索文章的配图（全新主线）。"""
import os
import csv
import io
import html
import math
import json
import statistics

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
CSV = os.path.join(ROOT, "data", "dse_results.csv")
META = os.path.join(ROOT, "data", "dse_meta.json")

INK = "#2C2C2A"
MUTED = "#5F5E5A"
GRID = "#D3D1C7"
BLUE = "#185FA5"
TEAL = "#0F6E56"
CORAL = "#993C1D"
AMBER = "#BA7517"
PURPLE = "#534AB7"
F_BLUE = "#E6F1FB"
F_TEAL = "#E1F5EE"
F_CORAL = "#FAECE7"
F_GRAY = "#F1EFE8"
F_AMBER = "#FAEEDA"

FONT = "Arial, 'Microsoft YaHei', sans-serif"

with io.open(CSV, encoding="utf-8") as f:
    ROWS = list(csv.DictReader(f))
with io.open(META, encoding="utf-8") as f:
    M = json.load(f)


def V(r, k):
    return float(r[k])


def pick(section, **kw):
    out = [r for r in ROWS if r["section"] == section]
    for k, v in kw.items():
        out = [r for r in out if str(r[k]) == str(v)]
    return out


RATE_AT = sorted(pick("rate", mode="at"), key=lambda r: -int(r["gap"]))
RATE_LT = sorted(pick("rate", mode="lt"), key=lambda r: -int(r["gap"]))
LAT0 = V(RATE_AT[0], "avg_lat_ns")
LAT1 = V(RATE_AT[-1], "avg_lat_ns")
LTLAT = V(RATE_LT[0], "avg_lat_ns")
TOPO4 = {r["topo"]: r for r in pick("topology") if r["gap"] == "4"}
ROUT = {(r["routing"], r["traffic"]): r for r in pick("routing")}
MSGLEN = {r["msg_bytes"]: r for r in pick("msglen")}
BANDW = {int(float(r["link_bw"])): r for r in pick("bandwidth")}
SAT_TPUT = round(V(TOPO4["mesh"], "tput_msg_per_us") / 100.0) * 100
K_AT, K_LT = M["kernel_at_ms"], M["kernel_lt_ms"]
K_RATIO = M["kernel_ratio"]
SWEEP_TOTAL = M["sweep_total_s"]


def e(s):
    return html.escape(str(s), quote=False)


def head(w, h, title, desc):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
            'viewBox="0 0 %d %d" role="img">\n<title>%s</title>\n<desc>%s</desc>\n'
            % (w, h, w, h, e(title), e(desc)))


def T(x, y, s, size=13, fill=INK, anchor="start", weight=400):
    return ('<text x="%.1f" y="%.1f" font-family="%s" font-size="%d" '
            'fill="%s" text-anchor="%s" font-weight="%d">%s</text>\n'
            % (x, y, FONT, size, fill, anchor, weight, e(s)))


def R(x, y, w, h, fill, stroke=GRID, rx=6, sw=0.8):
    return ('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%.1f" '
            'fill="%s" stroke="%s" stroke-width="%.1f"/>\n'
            % (x, y, w, h, rx, fill, stroke, sw))


def L(x1, y1, x2, y2, stroke=GRID, sw=1.0, dash=None):
    d = ' stroke-dasharray="%s"' % dash if dash else ""
    return ('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" '
            'stroke-width="%.1f"%s/>\n' % (x1, y1, x2, y2, stroke, sw, d))


def PL(pts, stroke, sw=2.2, dash=None):
    d = ' stroke-dasharray="%s"' % dash if dash else ""
    p = " ".join("%.1f,%.1f" % (x, y) for x, y in pts)
    return ('<polyline points="%s" fill="none" stroke="%s" stroke-width="%.1f" '
            'stroke-linejoin="round" stroke-linecap="round"%s/>\n'
            % (p, stroke, sw, d))


def C(x, y, r, fill, stroke="white", sw=1.0):
    return ('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s" stroke="%s" '
            'stroke-width="%.1f"/>\n' % (x, y, r, fill, stroke, sw))


def box(x, y, w, h, title, sub, fill, stroke, ts=14, ss=12):
    s = R(x, y, w, h, fill, stroke, 10, 0.9)
    s += T(x + 16, y + 24, title, ts, INK, weight=500)
    if sub:
        s += T(x + 16, y + 44, sub, ss, MUTED)
    return s


def save(name, body):
    path = os.path.join(ASSETS, name)
    with io.open(path, "w", encoding="utf-8") as f:
        f.write(body + "</svg>\n")
    print("wrote", name, len(body), "chars")


def arrow(x1, y1, x2, y2, color=MUTED):
    s = L(x1, y1, x2 - 7, y2, color, 1.6)
    s += ('<path d="M%.1f %.1f L%.1f %.1f L%.1f %.1f Z" fill="%s"/>\n'
          % (x2 - 8, y2 - 4, x2, y2, x2 - 8, y2 + 4, color))
    return s


# ---------------------------------------------------------------------------
# 1) 抽象层级决策：一条消息 = 一个事务
# ---------------------------------------------------------------------------
s = head(1000, 430, "抽象层级决策：一条消息一个事务",
         "架构探索模型把一条完整消息抽象成一个 tlm_generic_payload，丢掉逐周期流水状态机，但必须保留输出链路上的竞争与时间注解。")
s += T(40, 32, "抽象层级决策：一条消息 = 一个事务", 17, INK, weight=500)
s += T(40, 54, "丢的是实现细节，不是物理约束 —— 竞争、串行化、反压一样都不能少", 12, MUTED)

s += box(40, 76, 920, 62, "一笔事务 = 一条完整消息，端到端复用同一个 payload",
         "address = dst  ｜  data_length = msg_bytes  ｜  txn_ext: src / hops / inject_time / rt_choice",
         F_TEAL, TEAL, 15, 12)

s += T(40, 172, "丢掉（本层不回答的问题）", 14, MUTED, weight=500)
s += T(520, 172, "留下（否则模型退化成 LT）", 14, TEAL, weight=500)
drop = [
    "逐周期流水状态机 RC / VA / SA / ST",
    "VC 分配、可分离仲裁、HOL blocking",
    "逐片事务与大量扩展相位",
    "每周期唤醒的仲裁进程与 sc_clock",
]
keep = [
    "输出链路 = 单服务器（竞争）",
    "sc_time& 时间注解传递排队与串行化",
    "四相握手 + credit 流控 + PEQ",
    "analysis port / DMI / debug 统计旁路",
]
for i, txt in enumerate(drop):
    y = 190 + i * 32
    s += R(40, y, 440, 26, F_GRAY, GRID, 5, 0.6)
    s += T(54, y + 18, "×  " + txt, 12, MUTED)
for i, txt in enumerate(keep):
    y = 190 + i * 32
    s += R(520, y, 440, 26, F_TEAL, TEAL, 5, 0.6)
    s += T(534, y + 18, "✓  " + txt, 12, INK)

s += R(40, 336, 920, 68, F_BLUE, BLUE, 8, 0.8)
s += T(58, 362, "service = t_link + msg_bytes / link_bw       ｜       start = max(arrival + t_rc, link_free_at[op])", 13, INK)
s += T(58, 386, "一条消息占链路多久只取决于长度与带宽；后来的消息必须等 —— 这是架构探索能看见饱和点的全部原因。", 12, MUTED)
save("txn-abstraction.svg", s)


# ---------------------------------------------------------------------------
# 2) 链路单服务器模型 + 时间线
# ---------------------------------------------------------------------------
s = head(1000, 420, "链路单服务器服务模型与时间线",
         "事务 A 先占用输出链路，事务 B 到达时发现 link_free_at 尚未到期，于是排队；排队等待全部通过 delay 时间注解向下游传递。")
s += T(40, 32, "链路单服务器：拥塞从哪里来", 17, INK, weight=500)
s += T(40, 54, "每个输出端口维护一个「忙到何时」的时间戳，其余全是普通的时间注解算术", 12, MUTED)

s += box(40, 76, 200, 54, "事务到达", "arrival = now + delay", F_GRAY, MUTED, 14, 11)
s += arrow(240, 103, 278, 103)
s += box(280, 76, 210, 54, "路由计算", "op = route(dst)，+ t_rc", F_GRAY, MUTED, 14, 11)
s += arrow(490, 103, 528, 103)
s += box(530, 76, 220, 54, "链路排队", "start = max(arrival+t_rc, link_free_at[op])", F_AMBER, AMBER, 14, 11)
s += arrow(750, 103, 788, 103)
s += box(790, 76, 170, 54, "转发下一跳", "delay = done_at - now", F_TEAL, TEAL, 14, 11)

s += R(40, 146, 920, 46, F_BLUE, BLUE, 8, 0.8)
s += T(58, 176,
       "service = t_link + msg_bytes / link_bw       ｜       "
       "link_free_at[op] = start + service       ｜       "
       "queue_wait = start - arrival - t_rc",
       13, INK)

s += T(40, 224, "时间线示例：同一输出端口先后到达 A、B 两条消息（service = 5 ns）", 14, INK, weight=500)
x0, x1, unit = 90, 930, 70.0     # 1 ns = 70 px
base = 330
for t in range(0, 12):
    x = x0 + t * unit
    s += L(x, 246, x, base + 8, GRID, 0.7)
    s += T(x, base + 28, str(t), 11, MUTED, "middle")
s += L(x0, base, x1, base, INK, 1.2)
s += T(x1 + 4, base + 4, "ns", 11, MUTED)

# A: arrival 0, service 0..5
s += R(x0, 258, 5 * unit, 26, F_BLUE, BLUE, 5, 1.0)
s += T(x0 + 8, 277, "A 服务 0–5 ns", 12, INK)
# B: arrival 2, wait 2..5, service 5..10
s += R(x0 + 2 * unit, 292, 3 * unit, 26, F_CORAL, CORAL, 5, 1.0)
s += T(x0 + 2 * unit + 8, 311, "B 排队 2–5 ns（wait = 3）", 12, CORAL)
s += R(x0 + 5 * unit, 292, 5 * unit, 26, F_TEAL, TEAL, 5, 1.0)
s += T(x0 + 5 * unit + 8, 311, "B 服务 5–10 ns", 12, INK)
s += L(x0 + 2 * unit, 250, x0 + 2 * unit, base, CORAL, 1.0, "3,3")
s += T(x0 + 2 * unit, 246, "B 到达", 11, CORAL, "middle")
s += L(x0 + 5 * unit, 250, x0 + 5 * unit, base, TEAL, 1.0, "3,3")
s += T(x0 + 5 * unit, 246, "link_free_at 到期", 11, TEAL, "middle")

s += T(40, 386, "没有 wait()，没有时钟沿：B 的排队时间直接加进 delay，下游在 now+delay 语义上处理它",
       12, MUTED)
save("link-server-model.svg", s)


# ---------------------------------------------------------------------------
# 3) 注入率扫描：AT vs LT
# ---------------------------------------------------------------------------
rate_at = pick("rate", mode="at")
rate_lt = pick("rate", mode="lt")
gaps = [int(r["gap"]) for r in rate_at]
xs = [95 + i * 44 for i in range(len(gaps))]
lt_xs = [95 + i * 44 for i in range(len(rate_lt))]

s = head(1000, 440, "注入率扫描：AT 看得见饱和点，LT 看不见",
         "左图为平均延迟随注入间隔缩短的变化，AT 从 %.1f ns 升到 %.1f ns，LT 恒为 %.1f ns；右图为 AT 吞吐在约 %d msgs/us 封顶，最忙链路利用率同步升到 1.0。"
         % (LAT0, LAT1, LTLAT, SAT_TPUT))
s += T(40, 32, "注入率扫描：AT 看得见饱和点，LT 看不见", 17, INK, weight=500)
s += T(40, 54, "4×4 mesh ｜ XY ｜ uniform ｜ 64 B ｜ 16 B/ns ｜ 每节点 30 条消息（共 480 条）", 12, MUTED)

# left panel
X0, X1, Y0, Y1 = 80, 470, 92, 330
ymax = 80.0
s += T(80, 82, "平均端到端延迟 (ns)", 12, MUTED)
for v in (0, 20, 40, 60, 80):
    y = Y1 - v / ymax * (Y1 - Y0)
    s += L(X0, y, X1, y, GRID, 0.7)
    s += T(X0 - 8, y + 4, str(v), 11, MUTED, "end")
s += L(X0, Y0, X0, Y1, INK, 1.0)

at_pts = [(xs[i], Y1 - float(r["avg_lat_ns"]) / ymax * (Y1 - Y0))
          for i, r in enumerate(rate_at)]
lt_pts = [(lt_xs[i], Y1 - float(r["avg_lat_ns"]) / ymax * (Y1 - Y0))
          for i, r in enumerate(rate_lt)]
s += PL(lt_pts, TEAL, 2.2, "5,4")
s += PL(at_pts, BLUE, 2.6)
for x, y in at_pts:
    s += C(x, y, 3.6, BLUE)
for x, y in lt_pts:
    s += C(x, y, 3.0, TEAL)
s += T(at_pts[0][0] + 6, at_pts[0][1] - 12, "%.1f" % LAT0, 11, BLUE)
s += T(at_pts[-1][0] - 4, at_pts[-1][1] - 14, "%.1f" % LAT1, 11, BLUE, "end")
s += T(lt_pts[-1][0] - 4, lt_pts[-1][1] + 18,
       "LT %.1f 恒定" % LTLAT, 11, TEAL, "end")
for i, g in enumerate(gaps):
    s += T(xs[i], Y1 + 20, str(g), 11, MUTED, "middle")
s += T((X0 + X1) / 2, Y1 + 44, "注入间隔 gap (cycles) → 负载升高", 12, MUTED, "middle")

# right panel
RX0, RX1, RY0, RY1 = 570, 920, 92, 330
tput_max = 2700.0
s += T(570, 82, "AT 吞吐 (msgs/us) 与最忙链路利用率", 12, MUTED)
for v in (0, 900, 1800, 2700):
    y = RY1 - v / tput_max * (RY1 - RY0)
    s += L(RX0, y, RX1, y, GRID, 0.7)
    s += T(RX0 - 8, y + 4, str(v), 11, MUTED, "end")
for u in (0.0, 0.5, 1.0):
    y = RY1 - u * (RY1 - RY0)
    s += T(RX1 + 8, y + 4, "%.1f" % u, 11, CORAL)
bw = 22
rxs = [600 + i * 38 for i in range(len(gaps))]   # 柱状图横坐标，落在 RX0..RX1 内
tputs = [float(r["tput_msg_per_us"]) for r in rate_at]
utils = [float(r["max_link_util"]) for r in rate_at]
for i, v in enumerate(tputs):
    y = RY1 - v / tput_max * (RY1 - RY0)
    s += R(rxs[i] - bw / 2, y, bw, RY1 - y, F_BLUE, BLUE, 2, 0.7)
upts = [(rxs[i], RY1 - u * (RY1 - RY0)) for i, u in enumerate(utils)]
s += PL(upts, CORAL, 2.0, "4,3")
for x, y in upts:
    s += C(x, y, 2.8, CORAL)
s += L(RX0, RY1 - SAT_TPUT / tput_max * (RY1 - RY0),
       RX1, RY1 - SAT_TPUT / tput_max * (RY1 - RY0), AMBER, 1.4, "5,4")
s += T(RX0 + 6, RY1 - SAT_TPUT / tput_max * (RY1 - RY0) - 8,
       "饱和 ≈ %d msgs/us" % SAT_TPUT, 11, AMBER, "start")
for i, g in enumerate(gaps):
    s += T(rxs[i], RY1 + 20, str(g), 11, MUTED, "middle")
s += T((RX0 + RX1) / 2, RY1 + 44, "注入间隔 gap (cycles) → 负载升高", 12, MUTED, "middle")

s += R(40, 396, 920, 30, F_GRAY, GRID, 6, 0.6)
s += T(54, 416,
       "蓝柱 = AT 吞吐；红线 = 最忙链路利用率（右轴 0–1）。延迟拐点、吞吐封顶、链路满载三者落在同一个 gap≈4–6 区间。",
       12, INK)
save("dse-latency-throughput.svg", s)


# ---------------------------------------------------------------------------
# 4) 拓扑对比（对数横轴）
# ---------------------------------------------------------------------------
topo = pick("topology")
s = head(1000, 380, "拓扑对比：mesh / torus / ring",
         "同为 16 个节点，torus 因环绕链路把平均跳数从 3.67 降到 3.13，延迟与吞吐都优于 mesh；ring 平均跳数 5.27，延迟高出一个数量级。")
s += T(40, 32, "拓扑对比：环绕链路值多少", 17, INK, weight=500)
s += T(40, 54, "4×4 mesh / 4×4 torus / 16 节点 ring，同为 16 节点、XY 路由、uniform 流量", 12, MUTED)

lx, rx = 250.0, 700.0
lo, hi = 10.0, 400.0


def lxpos(v):
    return lx + (math.log10(max(v, lo)) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * (rx - lx)


for v in (10, 30, 100, 300):
    x = lxpos(v)
    s += L(x, 92, x, 300, GRID, 0.7)
    s += T(x, 320, str(v), 11, MUTED, "middle")
s += T((lx + rx) / 2, 344, "平均端到端延迟 (ns，对数轴)", 12, MUTED, "middle")

order = [("mesh", "4×4 mesh", BLUE, F_BLUE),
         ("torus", "4×4 torus", TEAL, F_TEAL),
         ("ring", "16 节点 ring", CORAL, F_CORAL)]
for gi, (key, label, col, fill) in enumerate(order):
    gy = 108 + gi * 66
    s += T(40, gy + 22, label, 14, INK, weight=500)
    for j, g in enumerate((8, 4)):
        r = [q for q in topo if q["topo"] == key and int(q["gap"]) == g][0]
        v = float(r["avg_lat_ns"])
        w = lxpos(v) - lx
        y = gy + j * 24
        s += R(lx, y, w, 19, fill, col, 3, 0.8)
        s += T(lx + w + 7, y + 14, "%.1f ns" % v, 11, col)
    s += T(40, gy + 38, "上：gap=8   下：gap=4", 10, MUTED)

s += R(760, 92, 200, 208, F_GRAY, GRID, 8, 0.7)
s += T(776, 116, "平均跳数 / 吞吐 (gap=4)", 12, INK, weight=500)
for i, (key, lab, col, _) in enumerate(order):
    r = [q for q in topo if q["topo"] == key and int(q["gap"]) == 4][0]
    y = 146 + i * 48
    s += T(776, y, lab, 12, col, weight=500)
    s += T(776, y + 18, "%s hops  ｜  %.0f msgs/us"
           % (r["avg_hops"], float(r["tput_msg_per_us"])), 11, MUTED)

s += R(40, 356, 920, 20, F_TEAL, TEAL, 4, 0.5)
s += T(52, 371, "结论：torus 在两种负载下都优于 mesh（%.1f vs %.1f ns @gap=4），代价是多一倍链路；ring 跳数暴涨，只适合极低成本场景。"
       % (V(TOPO4["torus"], "avg_lat_ns"), V(TOPO4["mesh"], "avg_lat_ns")),
       11, INK)
save("dse-topology.svg", s)


# ---------------------------------------------------------------------------
# 5) 路由 × 流量模式
# ---------------------------------------------------------------------------
rt = pick("routing")
s = head(1000, 430, "路由算法与流量模式对比",
         "确定性 XY/YX 在 transpose 与 bitcomplement 这类对抗性置换流量下明显劣化，逐包随机的 oblivious 路由能把延迟从约 95 ns 降到 63 ns；但在均匀与热点流量下反而更差。")
s += T(40, 32, "路由 × 流量：没有放之四海皆准的最优解", 17, INK, weight=500)
s += T(40, 54, "4×4 mesh ｜ gap=4 ｜ 64 B ｜ 16 B/ns ｜ 平均端到端延迟 (ns)", 12, MUTED)

Y0, Y1 = 92, 330
ymax = 240.0
for v in (0, 60, 120, 180, 240):
    y = Y1 - v / ymax * (Y1 - Y0)
    s += L(80, y, 950, y, GRID, 0.7)
    s += T(72, y + 4, str(v), 11, MUTED, "end")
s += L(80, Y0, 80, Y1, INK, 1.0)

traffic = [("uniform", "均匀"), ("transpose", "转置"),
           ("bitcomp", "位反"), ("hotspot", "热点")]
routing = [("xy", "XY", BLUE), ("yx", "YX", AMBER),
           ("oblivious", "Oblivious", TEAL)]
for i, (key, label, col) in enumerate(routing):
    cx = 786 + i * 58
    s += R(cx, 62, 14, 12, F_GRAY, col, 2, 1.0)
    s += T(cx + 19, 72, label, 11, MUTED)

for gi, (tr, label) in enumerate(traffic):
    gx = 120 + gi * 210
    for j, (key, _, col) in enumerate(routing):
        rr = [q for q in rt if q["routing"] == key and q["traffic"] == tr][0]
        v = float(rr["avg_lat_ns"])
        x = gx + j * 42
        y = Y1 - v / ymax * (Y1 - Y0)
        s += R(x, y, 32, Y1 - y, F_GRAY, col, 3, 1.0)
        s += R(x, y, 32, min(Y1 - y, 12), col, col, 3, 0.6)
        s += T(x + 16, y - 6, "%.0f" % v, 10, col, "middle")
    s += T(gx + 47, Y1 + 22, label, 12, INK, "middle")

s += R(40, 372, 920, 44, F_GRAY, GRID, 6, 0.6)
s += T(54, 390,
       "oblivious 把 transpose/bitcomp 从 %.1f/%.1f ns 降到 %.1f/%.1f ns（教科书结论），但均匀流量下从 %.1f 涨到 %.1f ns。"
       % (V(ROUT[("xy", "transpose")], "avg_lat_ns"),
          V(ROUT[("xy", "bitcomp")], "avg_lat_ns"),
          V(ROUT[("oblivious", "transpose")], "avg_lat_ns"),
          V(ROUT[("oblivious", "bitcomp")], "avg_lat_ns"),
          V(ROUT[("xy", "uniform")], "avg_lat_ns"),
          V(ROUT[("oblivious", "uniform")], "avg_lat_ns")), 12, INK)
s += T(54, 408, "结论：路由选择必须绑定目标 workload；这正是架构探索要回答、而单点仿真回答不了的问题。", 12, MUTED)
save("dse-routing-traffic.svg", s)


# ---------------------------------------------------------------------------
# 6) 消息长度 / 链路带宽
# ---------------------------------------------------------------------------
ms = pick("msglen")
bw = pick("bandwidth")
s = head(1000, 350, "消息长度与链路带宽敏感性",
         "服务时间 msg_bytes/link_bw 是唯一决定链路服务率的参数：32 B@16 B/ns 与 64 B@32 B/ns 得到完全相同的 %.2f ns 延迟。"
         % V(MSGLEN["32"], "avg_lat_ns"))
s += T(40, 32, "服务需求：消息长度与带宽是同一件事", 17, INK, weight=500)
s += T(40, 54, "4×4 mesh ｜ XY ｜ uniform ｜ gap=4 ｜ service = t_link + msg_bytes / link_bw", 12, MUTED)


def panel(x0, title, items, col, fill, note):
    out = T(x0, 96, title, 13, INK, weight=500)
    Y0, Y1, ymax = 116, 262, 250.0
    for v in (0, 100, 200, 250):
        y = Y1 - v / ymax * (Y1 - Y0)
        out += L(x0, y, x0 + 390, y, GRID, 0.7)
        out += T(x0 - 8, y + 4, str(v), 11, MUTED, "end")
    out += L(x0, Y0, x0, Y1, INK, 1.0)
    for i, (lab, v, extra) in enumerate(items):
        x = x0 + 26 + i * 92
        y = Y1 - v / ymax * (Y1 - Y0)
        out += R(x, y, 56, Y1 - y, fill, col, 3, 0.9)
        out += T(x + 28, y - 6, "%.1f" % v, 11, col, "middle")
        out += T(x + 28, Y1 + 20, lab, 11, INK, "middle")
        out += T(x + 28, Y1 + 36, extra, 10, MUTED, "middle")
    out += T(x0 + 195, 318, note, 11, MUTED, "middle")
    return out


s += panel(70, "消息长度（link_bw = 16 B/ns）",
           [(r["msg_bytes"] + " B", float(r["avg_lat_ns"]),
             "t_ser=%.0f ns" % (float(r["msg_bytes"]) / 16.0))
            for r in ms], BLUE, F_BLUE,
           "延迟 ∝ 消息长度，吞吐从 %.0f 降到 %.0f msgs/us"
           % (V(MSGLEN["32"], "tput_msg_per_us"), V(MSGLEN["256"], "tput_msg_per_us")))
s += panel(560, "链路带宽（msg_bytes = 64 B）",
           [(r["link_bw"].split(".")[0] + " B/ns", float(r["avg_lat_ns"]),
             "t_ser=%.0f ns" % (64.0 / float(r["link_bw"])))
            for r in bw], TEAL, F_TEAL,
           "32 B@16 与 64 B@32 得到完全相同的 %.2f ns"
           % V(MSGLEN["32"], "avg_lat_ns"))
save("dse-msg-bw.svg", s)


# ---------------------------------------------------------------------------
# 7) 架构探索闭环
# ---------------------------------------------------------------------------
s = head(1000, 300, "架构探索闭环",
         "参数矩阵经过事务级快速仿真与解析校准，产出架构决策；对少数关键点再用周期级模型或 RTL 复核。")
s += T(40, 32, "架构探索闭环：快模型负责搜索，慢模型负责验证", 17, INK, weight=500)
s += T(40, 54, "%d 轮扫描、单轮仿真内核约 %.1f ms，才跑得动「拓扑 × 路由 × 流量 × 注入率 × 消息长度」的组合"
       % (M["n_runs"], K_AT), 12, MUTED)

steps = [
    ("① 参数矩阵", ["topo / routing / traffic", "gap / 消息长度 / 带宽"], F_GRAY, MUTED),
    ("② 事务级仿真", ["link_free_at 服务模型", "时间注解 + PEQ"], F_BLUE, BLUE),
    ("③ 解析校准", ["零负载延迟 22 ns", "饱和吞吐、ρ≈1"], F_AMBER, AMBER),
    ("④ 架构决策", ["选拓扑、选路由", "定带宽与消息粒度"], F_TEAL, TEAL),
    ("⑤ 关键点复核", ["周期级模型 / RTL", "VC / arbiter / HOL"], F_CORAL, CORAL),
]
for i, (t, lines, fill, col) in enumerate(steps):
    x = 40 + i * 190
    s += box(x, 96, 166, 78, t, "", fill, col, 13, 11)
    s += T(x + 14, 140, lines[0], 10, MUTED)
    s += T(x + 14, 156, lines[1], 10, MUTED)
    if i < 4:
        s += arrow(x + 168, 135, x + 188, 135)

s += R(40, 200, 920, 62, F_GRAY, GRID, 8, 0.7)
s += T(56, 224, "能力边界：能回答拓扑 / 路由 / 容量 / 映射；不能回答 VC 数、arbiter 微架构、wormhole HOL、cycle 级尾延迟。", 12, INK)
s += T(56, 246, "AT 内核 %.2f ms vs LT %.2f ms（约 %.1f×）；%d 轮扫描主体 %.1f s，换来整条延迟-负载曲线与饱和点。"
       % (K_AT, K_LT, K_RATIO, M["n_runs"], SWEEP_TOTAL), 12, MUTED)
save("arch-exploration-loop.svg", s)

# ---------------------------------------------------------------------------
# 8) TLM 建模的基本思路
# ---------------------------------------------------------------------------
s = head(1000, 420, "TLM 建模的基本思路：通信 = 事务 + 时间注解",
         "发起方通过 socket 把一个 tlm_generic_payload 交给目标，时间不靠 wait 而靠随事务传递的 sc_time& delay 注解；AT 与 LT 是同一套接口上的两种时间编码风格。")
s += T(40, 32, "TLM 建模的基本思路：通信 = 事务 + 时间注解", 17, INK, weight=500)
s += T(40, 54, "不再描述信号与时钟沿，而是描述「谁在什么时候把什么交给谁」", 12, MUTED)

s += box(40, 80, 200, 60, "Initiator", "产生事务、携带 delay", F_GRAY, MUTED, 14, 11)
s += arrow(240, 110, 292, 110)
s += box(296, 80, 250, 60, "socket", "fw / bw 两个方向", F_BLUE, BLUE, 14, 11)
s += arrow(546, 110, 598, 110)
s += box(602, 80, 200, 60, "Target", "消费事务、回响应", F_GRAY, MUTED, 14, 11)
s += T(820, 104, "tlm_generic_payload", 12, INK, weight=500)
s += T(820, 124, "command / address / data", 11, MUTED)
s += T(820, 142, "extension / mm", 11, MUTED)

s += R(40, 152, 920, 42, F_TEAL, TEAL, 8, 0.8)
s += T(58, 178, "sc_time& delay 是时间支票：路由、排队、串行化都折算成偏移随事务走；不 wait、不需要 sc_clock", 13, INK)

s += box(40, 210, 440, 96, "AT：nb_transport_fw / bw + 四相 + PEQ",
         "", F_BLUE, BLUE, 14, 11)
s += T(58, 250, "非阻塞；能表达反压、排队与资源竞争", 12, MUTED)
s += T(58, 272, "BEGIN_REQ → END_REQ → BEGIN_RESP → END_RESP", 12, INK)
s += T(58, 294, "时间注解 + 扩展相位 + 时间事件队列", 12, MUTED)
s += box(520, 210, 440, 96, "LT：b_transport + quantum keeper",
         "", F_AMBER, AMBER, 14, 11)
s += T(538, 250, "阻塞；一次调用沿路径走到底，只累加时间", 12, MUTED)
s += T(538, 272, "trans.set_response_status(TLM_OK_RESPONSE)", 12, INK)
s += T(538, 294, "temporal decoupling：本地时间先跑，定期 sync", 12, MUTED)

for i, (t1, t2) in enumerate([
        ("extension", "ignorable / instance-specific"),
        ("memory manager", "payload 池、引用计数"),
        ("DMI / debug", "直访与无副作用快照"),
        ("analysis port", "零时序影响的性能旁路")]):
    x = 40 + i * 235
    s += R(x, 322, 220, 52, F_GRAY, GRID, 7, 0.6)
    s += T(x + 12, 344, t1, 12, INK, weight=500)
    s += T(x + 12, 362, t2, 11, MUTED)

s += T(40, 400, "本文两个模型共用同一套 TLM 接口与同一个 router spec，差别只在流控协议与时间模型。", 12, MUTED)
save("tlm-basics.svg", s)


# ---------------------------------------------------------------------------
# 9) Router 的基本原理
# ---------------------------------------------------------------------------
s = head(1000, 430, "Router 的基本原理：一条消息如何穿过一跳",
         "输入端口缓冲、路由计算、虚通道分配、交换分配、crossbar 穿越构成一跳；credit 流控保证下游有缓冲才发送；NoC 由 router 与链路按拓扑连起来。")
s += T(40, 32, "Router 的基本原理：一条消息如何穿过一跳", 17, INK, weight=500)
s += T(40, 54, "5 端口（N / E / S / W / L）结构，取自 ASPDAC 2006 的真实虚通道 router 架构", 12, MUTED)

s += R(40, 80, 300, 190, F_GRAY, GRID, 10, 0.9)
s += T(190, 104, "router（本跳）", 13, INK, "middle", 500)
for px, py, lab in ((56, 140, "W"), (146, 116, "N"),
                    (146, 208, "S"), (246, 140, "E")):
    s += R(px, py, 46, 26, F_BLUE, BLUE, 4, 0.7)
    s += T(px + 23, py + 18, lab, 11, INK, "middle")
s += R(112, 140, 116, 54, F_TEAL, TEAL, 6, 0.8)
s += T(170, 162, "输入缓冲 + 路由", 11, INK, "middle")
s += T(170, 180, "+ 仲裁 → XBAR", 11, INK, "middle")
s += arrow(228, 167, 248, 167, TEAL)
s += T(56, 254, "本地端口 L：接本节点的网络接口", 11, MUTED)
s += T(56, 272, "每条输出链路一次只服务一条消息", 11, MUTED)

s += T(370, 88, "一跳的四级流水（BookSim2 IQRouter）", 13, INK, weight=500)
stages = [
    ("RC 路由计算", "查目的坐标，定输出端口", F_BLUE, BLUE),
    ("VA 虚通道分配", "为消息申请下游通道", F_TEAL, TEAL),
    ("SA 交换分配", "仲裁谁本周期用 crossbar", F_AMBER, AMBER),
    ("ST 交换穿越", "经 crossbar 送到输出链路", F_CORAL, CORAL),
]
for i, (t1, t2, fill, col) in enumerate(stages):
    x = 370 + i * 155
    s += R(x, 100, 145, 62, fill, col, 7, 0.8)
    s += T(x + 12, 124, t1, 12, INK, weight=500)
    s += T(x + 12, 144, t2, 10, MUTED)
    if i < 3:
        s += arrow(x + 146, 131, x + 154, 131)

s += R(370, 180, 590, 76, F_GRAY, GRID, 8, 0.7)
s += T(388, 204, "credit 流控：上游先有信用才发", 12, INK, weight=500)
s += T(388, 226, "① NI 持有 ni_window 个 credit  →  ② 发一条消息消耗一个  →  ③ 下游缓冲释放后归还 credit", 11, MUTED)
s += T(388, 244, "没有信用就必须等；这保证不会向下游溢出，也让拥塞可被观测", 11, MUTED)

s += R(40, 292, 920, 60, F_BLUE, BLUE, 8, 0.8)
s += T(58, 318, "本文把四级流水合并成 t_rc，把输出链路看成单服务器：service = t_link + msg_bytes / link_bw", 13, INK)
s += T(58, 340, "一跳的绝对时刻 = arrival + t_rc + 排队等待 + service，全部折算进 TLM 的 delay 注解", 12, MUTED)

s += R(40, 362, 920, 46, F_TEAL, TEAL, 8, 0.7)
s += T(58, 390, "NoC = router + 链路：换拓扑只改连线，不改 router；换路由只改 route_port()，不改连线。", 13, INK)
save("router-basics.svg", s)


# ---------------------------------------------------------------------------
# 10) 两套流控协议与两种探索粒度
# ---------------------------------------------------------------------------
s = head(1000, 440, "AT credit 与 LT ready/valid：两套协议，两种探索粒度",
         "AT 用 credit 预约缓冲并有硬反压，能给出延迟-负载曲线与饱和点；LT 用 ready/valid 握手且下游恒 ready，只回答连通性与带宽是否够用。")
s += T(40, 32, "两套流控协议，两种探索粒度", 17, INK, weight=500)
s += T(40, 54, "同一份 router spec、同一条服务时间公式，差别在流控与时间模型", 12, MUTED)

s += R(40, 76, 440, 268, F_BLUE, BLUE, 10, 0.9)
s += T(60, 104, "AT  ｜  credit-based flow control", 14, INK, weight=500)
s += T(60, 126, "细粒度探索：延迟和拥塞怎么样", 12, BLUE, weight=500)
at_lines = [
    "NI 持有 ni_window 个 credit（初值 8）",
    "BEGIN_REQ：--credit，占用上游一个槽",
    "链路忙 → link_free_at[op] 决定排队",
    "LINK_DONE：服务完成，释放输入槽",
    "BEGIN_RESP ≡ 归还一个 credit",
    "END_RESP 闭合，credit 数守恒",
]
for i, t in enumerate(at_lines):
    s += T(60, 152 + i * 26, "•  " + t, 12, INK)
s += R(60, 312, 400, 24, F_GRAY, GRID, 4, 0.5)
s += T(70, 329, "实测：轻载 %.2f ns → 重载 %.2f ns，吞吐封顶" % (LAT0, LAT1), 11, INK)

s += R(520, 76, 440, 268, F_TEAL, TEAL, 10, 0.9)
s += T(540, 104, "LT  ｜  ready / valid handshake", 14, INK, weight=500)
s += T(540, 126, "粗粒度探索：连通性和带宽够不够", 12, TEAL, weight=500)
lt_lines = [
    "valid：上游声明本次事务有数据",
    "ready：下游声明本跳可以接收",
    "LT 没有队列，ready 恒为真",
    "没有重试，stalls = 0",
    "b_transport 一次走完整条路径",
    "delay 只累加每跳固定服务时间",
]
for i, t in enumerate(lt_lines):
    s += T(540, 152 + i * 26, "•  " + t, 12, INK)
s += R(540, 312, 400, 24, F_GRAY, GRID, 4, 0.5)
s += T(550, 329, "实测：延迟恒为 %.2f ns，对注入率不敏感" % LTLAT, 11, INK)

s += R(40, 360, 920, 60, F_GRAY, GRID, 8, 0.7)
s += T(58, 384, "连通性 / 带宽账 → 用 LT：路径几跳、每跳占链路多久、需求带宽是否超过链路服务率", 12, INK)
s += T(58, 406, "延迟 / 拥塞 → 用 AT：排队等待分布、饱和点、最忙链路、注入率-延迟曲线", 12, INK)
save("protocol-credit-vs-rv.svg", s)

print("done")
