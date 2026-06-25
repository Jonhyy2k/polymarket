#!/usr/bin/env python3
"""
Measure the REAL network RTT from this box to Polymarket (Cloudflare-fronted).
Run it FROM the deploy box (eu-west-1 / Ireland) to replace the latency *estimates*
with your actual numbers.

The path is: you -> nearest Cloudflare edge -> London origin -> back. So:
  - TCP-connect RTT  = you -> CF edge        (the leg you control; the clean floor)
  - first-byte (TTFB)= you -> edge -> origin -> back + processing  (the full round trip)
  - tick-to-react   ≈ this round trip + ~6 µs in-process (negligible) + engine match

There is no keyless way to time the true order/cancel RTT (that needs L2 auth —
gated). The network RTT below is the floor, and it dominates everything.

  python3 tools/measure_latency.py [--n 30]
"""
import argparse
import statistics as st
import subprocess

HOSTS = ["clob.polymarket.com", "ws-subscriptions-clob.polymarket.com"]
UA = "pm-latency/1"


def curl_w(host, fmt):
    r = subprocess.run(["curl", "-sS", "-o", "/dev/null", "-A", UA, "-w", fmt,
                        f"https://{host}/"], capture_output=True, text=True, timeout=20)
    return r.stdout.strip()


def pop(host):
    r = subprocess.run(["curl", "-sSI", "-A", UA, f"https://{host}/"],
                       capture_output=True, text=True, timeout=20)
    for line in r.stdout.splitlines():
        if line.lower().startswith("cf-ray:"):
            return line.split("-")[-1].strip()
    return "?"


def samples(host, key, n):
    vals = []
    for _ in range(n):
        try:
            v = float(curl_w(host, "%{" + key + "}"))
            if v > 0:
                vals.append(v * 1000.0)  # s -> ms
        except (ValueError, subprocess.SubprocessError):
            pass
    return sorted(vals)


def stat(xs):
    if not xs:
        return "  (no samples)"
    p95 = xs[min(len(xs) - 1, int(len(xs) * 0.95))]
    return f"min {xs[0]:6.2f}   median {st.median(xs):6.2f}   p95 {p95:6.2f}   ms"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=30)
    args = ap.parse_args()

    for host in HOSTS:
        print(f"\n=== {host} ===")
        print(f"Cloudflare PoP (cf-ray): {pop(host)}    (expect DUB from Ireland)")
        con = samples(host, "time_connect", args.n)
        tls = samples(host, "time_appconnect", args.n)
        ttfb = samples(host, "time_starttransfer", args.n)
        print(f"  TCP connect (you -> CF edge):        {stat(con)}")
        print(f"  TLS done:                            {stat(tls)}")
        print(f"  first byte (you->edge->London->back):{stat(ttfb)}")
        if con and ttfb:
            print(f"  -> clean network floor (TCP connect min) = {con[0]:.2f} ms  "
                  f"= round trip to the CF {pop(host)} edge")
            print(f"     full request incl. origin processing (TTFB) = {st.median(ttfb):.2f} ms "
                  f"= UPPER BOUND on the London round trip")

    print("\nHow to read it:")
    print("  - TCP connect (min) is the only CLEAN number: your round trip to the Cloudflare")
    print("    edge. From Ireland that should be ~1 ms.")
    print("  - The CF-edge -> London-origin -> back leg is hidden behind Cloudflare and can't")
    print("    be isolated keyless (TTFB mixes in origin processing). For the REAL feed leg,")
    print("    read the bot's '[FEED] deliv (ms)' line (one-way, trustworthy now you run chrony).")
    print("  - tick-to-react ≈ full London round trip + ~6 us in-process (negligible) + engine.")
    print("  - The true signed order/cancel RTT needs L2 auth (gated).")


if __name__ == "__main__":
    main()
