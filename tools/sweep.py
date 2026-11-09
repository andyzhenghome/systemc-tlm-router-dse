# -*- coding: utf-8 -*-
"""架构探索参数扫描：驱动 noc_txn_demo，汇总 CSV 行，并测 AT/LT 墙钟耗时。"""
import os
import subprocess
import time
import statistics
import json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CODE = os.path.join(ROOT, "code")
EXE_NAME = "noc_txn_demo.exe" if os.name == "nt" else "noc_txn_demo"
EXE = os.path.join(CODE, EXE_NAME)
OUT = os.path.join(ROOT, "data", "dse_results.csv")

COLS = ["mode", "topo", "routing", "traffic", "gap", "msg_bytes", "link_bw",
        "avg_lat_ns", "p95_lat_ns", "max_lat_ns", "tput_msg_per_us",
        "avg_queue_ns", "max_queue_ns", "queued_frac", "max_link_util",
        "avg_hops", "n_eject", "n_drop", "wall_ms", "section"]

rows = []
T0 = time.perf_counter()


def run(section, mode="at", mx=4, my=4, nmsg=30, gap=8,
        routing="xy", topo="mesh", traffic="uniform", msg=64, bw=16):
    cmd = [EXE, mode, str(mx), str(my), str(nmsg), str(gap),
           routing, topo, traffic, str(msg), str(bw)]
    p = subprocess.run(cmd, cwd=CODE, capture_output=True, text=True, timeout=120)
    line = None
    for ln in p.stdout.splitlines():
        if ln.startswith("CSV:"):
            line = ln[4:].strip()
    if line is None:
        print("  FAILED:", " ".join(cmd), "\n", p.stdout[-400:], p.stderr[-400:])
        return None
    vals = line.split(",")
    r = dict(zip(COLS[:19], vals))
    r["section"] = section
    rows.append(r)
    return r


def show(r):
    if r is None:
        return
    print("  {:<10} {:<6} {:<10} {:<9} gap={:<3} msg={:<4} bw={:<3} "
          "lat={:<9} tput={:<10} util={}".format(
              r["section"], r["mode"], r["topo"], r["traffic"],
              r["gap"], r["msg_bytes"], r["link_bw"],
              r["avg_lat_ns"][:7], r["tput_msg_per_us"][:9], r["max_link_util"][:6]))


print("[1/4] 注入率扫描：AT vs LT")
for mode in ("at", "lt"):
    for g in (64, 32, 16, 12, 8, 6, 4, 2, 1):
        show(run("rate", mode=mode, gap=g))

print("[2/4] 拓扑对比：mesh / torus / ring")
for g in (8, 4):
    for t in ("mesh", "torus"):
        show(run("topology", gap=g, topo=t))
    show(run("topology", mx=16, my=1, gap=g, topo="ring"))

print("[3/4] 路由 × 流量模式（mesh, gap=4）")
for rt in ("xy", "yx", "oblivious"):
    for tr in ("uniform", "transpose", "bitcomp", "hotspot"):
        show(run("routing", gap=4, routing=rt, traffic=tr))

print("[4/4] 消息长度 / 链路带宽敏感性（mesh, gap=4）")
for m in (32, 64, 128, 256):
    show(run("msglen", gap=4, msg=m))
for b in (8, 16, 32):
    show(run("bandwidth", gap=4, bw=b))

sweep_total = time.perf_counter() - T0   # 43 轮扫描主体，不含后面的计时基准

# sc_start 内部耗时：同一配置重复 9 次取最小值（最小 = 受系统干扰最少）
print("[wall] AT / LT 仿真内核耗时（sc_start，9 次取最小）")


def bench_kernel(mode, n=9):
    vals = []
    args = [mode, "4", "4", "30", "4", "xy", "mesh", "uniform", "64", "16"]
    for _ in range(n):
        p = subprocess.run([EXE] + args, cwd=CODE,
                           capture_output=True, text=True, timeout=120)
        for ln in p.stdout.splitlines():
            if ln.startswith("CSV:"):
                vals.append(float(ln[4:].strip().split(",")[18]))
                break
    return min(vals) if vals else 0.0


wall = {"at": bench_kernel("at"), "lt": bench_kernel("lt")}
print("  at min = {:.3f} ms, lt min = {:.3f} ms".format(wall["at"], wall["lt"]))
print("  ratio AT/LT = {:.1f}x".format(wall["at"] / max(wall["lt"], 1e-9)))

with open(OUT, "w", encoding="utf-8") as f:
    f.write(",".join(COLS) + "\n")
    for r in rows:
        f.write(",".join(str(r[c]) for c in COLS) + "\n")
print("\nwrote {} ({} runs)".format(OUT, len(rows)))
print("wall_at={:.6f} wall_lt={:.6f} ratio={:.3f}".format(
    wall["at"], wall["lt"], wall["at"] / wall["lt"]))

# 进程级：AT / LT 单次运行耗时（同一 4x4 / 30 per node）
def proc_med(exe, args, n=7):
    cmd = [os.path.join(CODE, exe)] + args
    ts = []
    for _ in range(n):
        t0 = time.perf_counter()
        subprocess.run(cmd, cwd=CODE, capture_output=True, timeout=180)
        ts.append(time.perf_counter() - t0)
    return min(ts)


txn = proc_med(EXE_NAME,
               ["at", "4", "4", "30", "4", "xy", "mesh", "uniform", "64", "16"])
lt = proc_med(EXE_NAME,
              ["lt", "4", "4", "30", "4", "xy", "mesh", "uniform", "64", "16"])

meta = {
    "kernel_at_ms": wall["at"], "kernel_lt_ms": wall["lt"],
    "kernel_ratio": wall["at"] / max(wall["lt"], 1e-9),
    "proc_txn_at_s": txn, "proc_txn_lt_s": lt,
    "sweep_total_s": sweep_total,
    "n_runs": len(rows),
}
META = os.path.join(ROOT, "data", "dse_meta.json")
with open(META, "w", encoding="utf-8") as f:
    json.dump(meta, f, indent=2)
print("wrote", META)
print("process: txn_at={:.4f}s lt={:.4f}s | {} runs in {:.2f}s".format(
    txn, lt, len(rows), sweep_total))
