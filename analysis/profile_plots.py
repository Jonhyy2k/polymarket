#!/usr/bin/env python3
"""
Analyze a profiled arb_detector run: per-frame latency (replay CSV) + system
metrics (sysmetrics CSV from profile_run.py). Produces two figures and prints a
findings report (tail, outliers + what they were, memory growth, per-core CPU,
context-switch jitter, network).

  python3 analysis/profile_plots.py
"""
import csv
import statistics as st
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

REPLAY = "logs/replay_mocklive.csv"
SYS = "logs/sysmetrics.csv"


def pct(xs, q):
    if not xs:
        return 0.0
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * q))]


# ---- load per-frame latency (dedup by frame_id; t in µs) ----
frames = {}
with open(REPLAY) as f:
    for r in csv.DictReader(f):
        fid = r["frame_id"]
        if fid in frames:
            continue
        try:
            t0, t1, t2, t3 = (int(r["t0_recv_ns"]), int(r["t1_parse_start_ns"]),
                              int(r["t2_parse_done_ns"]), int(r["t3_books_done_ns"]))
        except (KeyError, ValueError):
            continue
        if not (t3 > t0 > 0 and t2 >= t1 >= t0):
            continue
        ev = int(r["book_events"]) + int(r["price_change_events"]) + int(r["bbo_events"]) + int(r["trade_events"])
        frames[fid] = {"t0": t0, "queue": (t1 - t0) / 1e3, "parse": (t2 - t1) / 1e3,
                       "book": (t3 - t2) / 1e3, "e2e": (t3 - t0) / 1e3,
                       "bytes": int(r["frame_bytes"]), "events": ev}

F = sorted(frames.values(), key=lambda d: d["t0"])
if not F:
    print("no usable frames in replay CSV"); raise SystemExit(1)
t_origin = F[0]["t0"]
for d in F:
    d["ts"] = (d["t0"] - t_origin) / 1e9
e2e = [d["e2e"] for d in F]
qa, pa, ba = [d["queue"] for d in F], [d["parse"] for d in F], [d["book"] for d in F]

# ---- load system metrics ----
try:
    S = list(csv.DictReader(open(SYS)))
except FileNotFoundError:
    S = []


def col(rows, c, f=float):
    return [f(r[c]) for r in rows if c in r and r[c] != ""]


# ================= FINDINGS REPORT =================
print(f"\n==== LATENCY (per-frame, {len(F)} frames over {F[-1]['ts']:.0f}s) ====")
print(f"{'stage':8s} {'p50':>8s} {'p90':>8s} {'p99':>8s} {'p99.9':>8s} {'max':>9s}  (µs)")
for name, xs in [("queue", qa), ("parse", pa), ("book", ba), ("e2e", e2e)]:
    print(f"{name:8s} {pct(xs,.5):8.2f} {pct(xs,.9):8.2f} {pct(xs,.99):8.2f} "
          f"{pct(xs,.999):8.2f} {max(xs):9.2f}")

# outliers: top 12 worst e2e, classify by frame size
print("\n==== TOP 12 e2e OUTLIERS (where/what) ====")
print(f"{'t_s':>7s} {'e2e_us':>9s} {'queue':>7s} {'parse':>7s} {'book':>7s} {'bytes':>7s} {'events':>6s}  dominant")
worst = sorted(F, key=lambda d: -d["e2e"])[:12]
big_tail = 0
for d in worst:
    stage = max(("queue", d["queue"]), ("parse", d["parse"]), ("book", d["book"]), key=lambda x: x[1])
    if d["bytes"] > 4000:
        big_tail += 1
    print(f"{d['ts']:7.1f} {d['e2e']:9.2f} {d['queue']:7.2f} {d['parse']:7.2f} {d['book']:7.2f} "
          f"{d['bytes']:7d} {d['events']:6d}  {stage[0]}")
print(f"  -> {big_tail}/12 of the worst frames are big (>4KB, initial-dump/snapshot); "
      f"{12-big_tail}/12 are small frames slowed by jitter (preemption/cache).")

# size correlation
big = [d["e2e"] for d in F if d["bytes"] > 4000]
small = [d["e2e"] for d in F if d["bytes"] <= 4000]
print(f"\n  e2e p50 small frames (≤4KB): {pct(small,.5):.2f}µs | big frames (>4KB): "
      f"{pct(big,.5) if big else 0:.2f}µs  ({len(big)} big / {len(small)} small)")

if S:
    rss = col(S, "rss_kb"); vmd = col(S, "vmdata_kb"); vstk = col(S, "vmstk_kb")
    vol = col(S, "vol_ctxt"); nvol = col(S, "nonvol_ctxt"); ts = col(S, "t_s")
    print(f"\n==== MEMORY ({len(S)} samples over {ts[-1]:.0f}s) ====")
    print(f"  RSS:   {min(rss)/1024:.1f} -> {max(rss)/1024:.1f} MB (Δ {(rss[-1]-rss[0])/1024:+.2f} MB)  "
          f"{'LEAK?' if rss[-1]-rss[0] > 8192 else 'stable'}")
    print(f"  heap(VmData): {min(vmd)/1024:.1f}->{max(vmd)/1024:.1f} MB | stack(VmStk): {max(vstk)} KB")
    dvol = vol[-1] - vol[0]; dnvol = nvol[-1] - nvol[0]
    print(f"\n==== CONTEXT SWITCHES (jitter) over {ts[-1]:.0f}s ====")
    print(f"  voluntary: {dvol} ({dvol/ts[-1]:.0f}/s) | NON-voluntary (preemption): {dnvol} "
          f"({dnvol/ts[-1]:.1f}/s)  <- the latency-tail driver")
    for c in ("cpu2", "cpu4", "cpu6", "sender_cpu_est"):
        v = col(S, c)
        if v:
            print(f"  {c:14s} avg={st.mean(v):5.0f}%  max={max(v):3.0f}%")

# ================= FIGURE 1: LATENCY =================
fig, ax = plt.subplots(2, 2, figsize=(15, 9))
fig.suptitle("In-process latency profile — 5-min live capture (host µs; network ms dominates in prod)",
             fontweight="bold")
tsx = [d["ts"] for d in F]
ax[0, 0].scatter(tsx, e2e, s=3, alpha=0.3, color="#1565c0")
thr = pct(e2e, .99)
ox = [d["ts"] for d in F if d["e2e"] >= thr]; oy = [d["e2e"] for d in F if d["e2e"] >= thr]
ax[0, 0].scatter(ox, oy, s=12, color="#c62828", label=f"top 1% (≥{thr:.0f}µs)")
ax[0, 0].set_yscale("log"); ax[0, 0].set_xlabel("seconds"); ax[0, 0].set_ylabel("e2e µs (log)")
ax[0, 0].set_title("A. e2e latency over time (red = tail)"); ax[0, 0].legend(fontsize=8)

for name, xs, c in [("e2e", e2e, "#1565c0"), ("parse", pa, "#2e7d32"),
                    ("book", ba, "#ef6c00"), ("queue", qa, "#6a1b9a")]:
    sx = sorted(xs); n = len(sx)
    surv = [1 - i / n for i in range(n)]
    ax[0, 1].plot(sx, surv, label=name, color=c)
ax[0, 1].set_xscale("log"); ax[0, 1].set_yscale("log")
ax[0, 1].set_xlabel("µs (log)"); ax[0, 1].set_ylabel("P(X > x)")
ax[0, 1].set_title("B. survival / tail (log-log)"); ax[0, 1].legend(fontsize=8); ax[0, 1].grid(alpha=.3, which="both")

bytesx = [d["bytes"] for d in F]
sc = ax[1, 0].scatter(bytesx, e2e, s=4, alpha=0.3, c=[d["events"] for d in F], cmap="viridis")
ax[1, 0].set_yscale("log"); ax[1, 0].set_xscale("log")
ax[1, 0].set_xlabel("frame bytes (log)"); ax[1, 0].set_ylabel("e2e µs (log)")
ax[1, 0].set_title("C. e2e vs frame size (color = #events)"); fig.colorbar(sc, ax=ax[1, 0], label="events")

labels = ["queue", "parse", "book", "e2e"]
p50s = [pct(x, .5) for x in (qa, pa, ba, e2e)]
p99s = [pct(x, .99) for x in (qa, pa, ba, e2e)]
p999 = [pct(x, .999) for x in (qa, pa, ba, e2e)]
import numpy as np
xpos = np.arange(len(labels)); w = .26
ax[1, 1].bar(xpos - w, p50s, w, label="p50", color="#90caf9")
ax[1, 1].bar(xpos, p99s, w, label="p99", color="#1565c0")
ax[1, 1].bar(xpos + w, p999, w, label="p99.9", color="#c62828")
ax[1, 1].set_xticks(xpos); ax[1, 1].set_xticklabels(labels); ax[1, 1].set_yscale("log")
ax[1, 1].set_ylabel("µs (log)"); ax[1, 1].set_title("D. per-stage percentiles (where the tail lives)")
ax[1, 1].legend(fontsize=8)
fig.tight_layout(rect=[0, 0, 1, 0.96])
fig.savefig("analysis/profile_latency.png", dpi=110)
print("\nsaved analysis/profile_latency.png")

# ================= FIGURE 2: SYSTEM =================
if S:
    fig2, bx = plt.subplots(2, 2, figsize=(15, 9))
    fig2.suptitle("System profile — memory, per-core CPU, context-switch jitter, network",
                  fontweight="bold")
    bx[0, 0].plot(ts, [v / 1024 for v in rss], label="RSS", color="#1565c0")
    bx[0, 0].plot(ts, [v / 1024 for v in vmd], label="heap (VmData)", color="#2e7d32")
    bx[0, 0].set_xlabel("seconds"); bx[0, 0].set_ylabel("MB")
    bx[0, 0].set_title("E. memory over time (flat = no leak)"); bx[0, 0].legend(fontsize=8); bx[0, 0].grid(alpha=.3)

    for c, lab, col_ in [("cpu2", "core2 receiver", "#6a1b9a"), ("cpu4", "core4 parser", "#c62828"),
                         ("cpu6", "core6 logger", "#ef6c00"), ("sender_cpu_est", "sender(float)", "#1565c0")]:
        v = col(S, c)
        if v:
            bx[0, 1].plot(ts[:len(v)], v, label=lab, lw=1)
    bx[0, 1].set_xlabel("seconds"); bx[0, 1].set_ylabel("CPU %")
    bx[0, 1].set_title("F. per-core CPU (parser+sender busy-poll = pegged)"); bx[0, 1].legend(fontsize=8)

    rate = [0] + [max(0, (nvol[i] - nvol[i - 1] + vol[i] - vol[i - 1]) / max(ts[i] - ts[i - 1], 1e-6))
                  for i in range(1, len(ts))]
    nrate = [0] + [max(0, (nvol[i] - nvol[i - 1]) / max(ts[i] - ts[i - 1], 1e-6)) for i in range(1, len(ts))]
    bx[1, 0].plot(ts, rate, label="all ctxt-sw/s", color="#90a4ae", lw=1)
    bx[1, 0].plot(ts, nrate, label="involuntary/s (preempt)", color="#c62828", lw=1)
    bx[1, 0].set_xlabel("seconds"); bx[1, 0].set_ylabel("switches/s")
    bx[1, 0].set_title("G. context-switch rate (jitter source)"); bx[1, 0].legend(fontsize=8); bx[1, 0].grid(alpha=.3)

    bx[1, 1].plot(ts, col(S, "rx_kbps"), label="rx", color="#1565c0", lw=1)
    bx[1, 1].plot(ts, col(S, "tx_kbps"), label="tx", color="#ef6c00", lw=1)
    bx[1, 1].set_xlabel("seconds"); bx[1, 1].set_ylabel("kbit/s")
    bx[1, 1].set_title("H. network throughput (feed)"); bx[1, 1].legend(fontsize=8); bx[1, 1].grid(alpha=.3)
    fig2.tight_layout(rect=[0, 0, 1, 0.96])
    fig2.savefig("analysis/profile_system.png", dpi=110)
    print("saved analysis/profile_system.png")
