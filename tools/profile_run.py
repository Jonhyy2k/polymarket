#!/usr/bin/env python3
"""
Profile an arb_detector run on real hardware: launch it, sample /proc at a fixed
interval (memory RSS/heap/stack, per-core CPU%, context switches, network), then
SIGINT for a clean shutdown. Writes a system-metrics CSV; the binary itself logs
per-frame latency (t0..t3) to its replay CSV. Analyze both with profile_plots.py.

  python3 tools/profile_run.py --config config.mocklive.json --duration 300

Threads are pinned (config): receiver->core2, parser->core4(main thread),
logger->core6; sender/reward-refresh float. So core 2/4/6 utilisation attributes
directly to receiver/parser/logger.
"""
import argparse
import csv
import os
import signal
import subprocess
import time

CLK = os.sysconf("SC_CLK_TCK")


def read_status(pid):
    keys = ("VmRSS", "VmData", "VmStk", "VmHWM", "Threads",
            "voluntary_ctxt_switches", "nonvoluntary_ctxt_switches")
    out = {}
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                k, _, v = line.partition(":")
                if k in keys:
                    out[k] = int(v.strip().split()[0])
    except FileNotFoundError:
        return None
    return out


def read_cpu_stat():
    cores = {}
    with open("/proc/stat") as f:
        for line in f:
            if line.startswith("cpu") and len(line) > 3 and line[3].isdigit():
                p = line.split()
                vals = list(map(int, p[1:8]))  # user,nice,sys,idle,iowait,irq,softirq
                idle = vals[3] + vals[4]
                cores[int(p[0][3:])] = (sum(vals) - idle, sum(vals))
    return cores


def read_threads(pid):
    res = {}
    try:
        tids = os.listdir(f"/proc/{pid}/task")
    except FileNotFoundError:
        return res
    for tid in tids:
        try:
            with open(f"/proc/{pid}/task/{tid}/stat") as f:
                line = f.read()
            rest = line[line.rindex(")") + 2:].split()
            res[tid] = (int(rest[11]) + int(rest[12]), int(rest[36]))  # utime+stime, last CPU
        except (FileNotFoundError, ValueError, IndexError):
            continue
    return res


def pick_iface():
    best, bestrx = "lo", -1
    with open("/proc/net/dev") as f:
        for line in f:
            if ":" in line and not line.strip().startswith(("Inter", "face")):
                name = line.split(":")[0].strip()
                if name == "lo":
                    continue
                rx = int(line.split(":")[1].split()[0])
                if rx > bestrx:
                    best, bestrx = name, rx
    return best


def read_net(iface):
    with open("/proc/net/dev") as f:
        for line in f:
            if line.strip().startswith(iface + ":"):
                p = line.split(":")[1].split()
                return int(p[0]), int(p[8])  # rx_bytes, tx_bytes
    return 0, 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="config.mocklive.json")
    ap.add_argument("--bin", default="./build/arb_detector")
    ap.add_argument("--duration", type=float, default=300)
    ap.add_argument("--interval", type=float, default=0.2)
    ap.add_argument("--out", default="logs/sysmetrics.csv")
    ap.add_argument("--applog", default="logs/profile_app.log")
    args = ap.parse_args()

    iface = pick_iface()
    ncores = os.cpu_count()
    applog = open(args.applog, "w")
    proc = subprocess.Popen([args.bin, "--config", args.config],
                            stdout=applog, stderr=subprocess.STDOUT)
    pid = proc.pid
    print(f"launched pid={pid} iface={iface} cores={ncores} dur={args.duration}s")
    time.sleep(1.0)  # let threads spawn / connect

    prev_cores = read_cpu_stat()
    prev_threads = read_threads(pid)
    prev_rx, prev_tx = read_net(iface)
    prev_t = time.monotonic()

    header = (["t_s", "rss_kb", "vmdata_kb", "vmstk_kb", "threads", "vol_ctxt", "nonvol_ctxt",
               "rx_kbps", "tx_kbps"] + [f"cpu{c}" for c in range(ncores)] + ["sender_cpu_est"])
    rows = []
    t0 = time.monotonic()
    while True:
        now = time.monotonic()
        if now - t0 >= args.duration or proc.poll() is not None:
            break
        st = read_status(pid)
        if st is None:
            break
        cores, threads = read_cpu_stat(), read_threads(pid)
        rx, tx = read_net(iface)
        dt = max(now - prev_t, 1e-6)

        core_pct = []
        for c in range(ncores):
            b0, t_0 = prev_cores.get(c, (0, 0))
            b1, t_1 = cores.get(c, (0, 0))
            core_pct.append(100.0 * (b1 - b0) / (t_1 - t_0) if (t_1 - t_0) > 0 else 0.0)

        sender_ticks = 0  # threads not on a pinned core (sender/reward-refresh)
        for tid, (ticks, p) in threads.items():
            d = ticks - prev_threads.get(tid, (ticks, p))[0]
            if p not in (2, 4, 6) and d > 0:
                sender_ticks += d

        rows.append([round(now - t0, 3), st.get("VmRSS", 0), st.get("VmData", 0),
                     st.get("VmStk", 0), st.get("Threads", 0),
                     st.get("voluntary_ctxt_switches", 0), st.get("nonvoluntary_ctxt_switches", 0),
                     round((rx - prev_rx) * 8 / 1000.0 / dt, 1),
                     round((tx - prev_tx) * 8 / 1000.0 / dt, 1)]
                    + [round(x, 1) for x in core_pct]
                    + [round(100.0 * sender_ticks / (CLK * dt), 1)])
        prev_cores, prev_threads, prev_rx, prev_tx, prev_t = cores, threads, rx, tx, now
        time.sleep(args.interval)

    print("sending SIGINT for clean shutdown...")
    try:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=20)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        proc.kill()
    applog.close()
    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)
    print(f"wrote {len(rows)} samples -> {args.out}  (app log -> {args.applog})")


if __name__ == "__main__":
    main()
