#!/usr/bin/env python3
# ILLUSTRATIVE returns model for the Polymarket LP-rewards maker — NOT A FORECAST.
#
# Every number here is an ASSUMPTION you can change. There is no measured net-return
# distribution yet; that must come from the live quote_telemetry.csv capture. This
# only shows the SHAPE of outcomes and how violently they depend on the ONE big
# unknown — adverse selection (how often a TRUE LP gets picked off once filled).
#
# Model per day, per Monte-Carlo path:
#   gross $/day  = capital * gross_frac(capital)        (capacity-bound; fit to repo table)
#                  capped at a plausible max share of the thin reward pools
#                  * competition-compression decay over time
#   net  $/day   = gross * adverse_mult  + noise  - rare_inventory_shock
#   capital     += net                                  (reinvest / compound)

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

rng = np.random.default_rng(7)

# ----------------------------- ASSUMPTIONS -----------------------------
START = 860.0      # €800 ≈ $860 starting capital
DAYS  = 180        # ~6 months
N     = 30000      # Monte-Carlo paths

# Gross reward %/day vs capital, fit to the repo's capacity-bound table
# (16%/d @ $860 -> 12% @ $3k -> 7% @ $10k -> 3.5% @ $30k):  2.94 * cap^-0.43
def gross_frac(cap):
    return 2.94 * np.power(cap, -0.43)

POOL_CEIL_USD      = 400.0   # thin pools saturate: hard cap on $/day you can capture
COMP_HALFLIFE_DAYS = 90.0    # snapshot reward rate gets competed away (halves in ~90d)
comp = 0.5 ** (1.0 / COMP_HALFLIFE_DAYS)

# The ONE big unknown: net = gross * adverse-selection multiplier.
# 'tail' = prob/day of a bad-inventory day that wipes several days of reward.
REGIMES = {
    "Optimistic (ACR holds, fills rare)": dict(mean=0.75, sd=0.20, tail=0.01, loss=(2, 5), color="#2e7d32"),
    "Base (moderate adverse selection)":  dict(mean=0.45, sd=0.35, tail=0.04, loss=(3, 7), color="#1565c0"),
    "Pessimistic (true-LP picked off)":   dict(mean=0.25, sd=0.55, tail=0.07, loss=(3, 8), color="#c62828"),
}
# -----------------------------------------------------------------------


def simulate(p):
    cap = np.full(N, START)
    paths = np.empty((DAYS + 1, N)); paths[0] = cap
    peak = cap.copy(); maxdd = np.zeros(N)
    for d in range(1, DAYS + 1):
        # gross $/day = 2.94*cap^0.57 (product form of cap*gross_frac — finite at cap=0),
        # pool-capped, then decayed by competition (decay shrinks the capturable pool too).
        gross = np.minimum(2.94 * np.power(cap, 0.57), POOL_CEIL_USD) * (comp ** d)
        net = gross * p["mean"] + gross * p["sd"] * rng.standard_normal(N)
        shock = (rng.random(N) < p["tail"]) * gross * rng.uniform(p["loss"][0], p["loss"][1], N)
        cap = np.maximum(cap + net - shock, 0.0)
        paths[d] = cap
        peak = np.maximum(peak, cap)
        maxdd = np.maximum(maxdd, (peak - cap) / np.maximum(peak, 1e-9))
    return paths, cap, maxdd


results = {name: simulate(p) for name, p in REGIMES.items()}

# ----------------------------- numbers ---------------------------------
def pct(x, q): return float(np.percentile(x, q))
print(f"\nStart ${START:.0f}  |  horizon {DAYS} days  |  {N:,} paths   (ILLUSTRATIVE — not a forecast)\n")
hdr = f"{'regime':37s} {'median':>9s} {'p5':>8s} {'p95':>9s} {'P(profit)':>9s} {'P(2x)':>6s} {'P(loss)':>8s} {'P(<½)':>6s} {'medDD':>6s}"
print(hdr); print("-" * len(hdr))
stats = {}
for name, (paths, final, maxdd) in results.items():
    s = dict(median=np.median(final), p5=pct(final, 5), p95=pct(final, 95),
             p_profit=np.mean(final > START), p_2x=np.mean(final > 2 * START),
             p_loss=np.mean(final < START), p_half=np.mean(final < 0.5 * START),
             med_dd=np.median(maxdd))
    stats[name] = s
    print(f"{name:37s} ${s['median']:8.0f} ${s['p5']:7.0f} ${s['p95']:8.0f} "
          f"{s['p_profit']*100:8.0f}% {s['p_2x']*100:5.0f}% {s['p_loss']*100:7.0f}% "
          f"{s['p_half']*100:5.0f}% {s['med_dd']*100:5.0f}%")
print()

# ----------------------------- plots -----------------------------------
fig = plt.figure(figsize=(15, 10))
fig.suptitle("€800 LP-rewards maker — ILLUSTRATIVE outcome model (assumptions, NOT a forecast)\n"
             "The spread between panels is driven by ONE unmeasured unknown: adverse selection. "
             "Real numbers come from the live telemetry capture.",
             fontsize=12, fontweight="bold")
days = np.arange(DAYS + 1)

# Panel A: fan chart (Base) + regime medians
axA = fig.add_subplot(2, 2, 1)
base_name = "Base (moderate adverse selection)"
bpaths = results[base_name][0]
loB, q25, med, q75, hiB = (np.percentile(bpaths, q, axis=1) for q in (5, 25, 50, 75, 95))
axA.fill_between(days, loB, hiB, color="#1565c0", alpha=0.15, label="Base 5–95%")
axA.fill_between(days, q25, q75, color="#1565c0", alpha=0.30, label="Base 25–75%")
for name, (paths, _, _) in results.items():
    axA.plot(days, np.percentile(paths, 50, axis=1), color=REGIMES[name]["color"],
             lw=2, label=f"median — {name.split('(')[0].strip()}")
axA.axhline(START, color="k", ls=":", lw=1, label="start €800")
axA.set_yscale("log"); axA.set_xlabel("day"); axA.set_ylabel("capital $ (log)")
axA.set_title("A.  Capital over time — median by regime + Base fan"); axA.legend(fontsize=7, loc="upper left")
axA.grid(alpha=0.3, which="both")

# Panel B: final-capital distribution (Base)
axB = fig.add_subplot(2, 2, 2)
bf = np.clip(results[base_name][1], 10, None)
axB.hist(bf, bins=np.logspace(np.log10(10), np.log10(max(bf.max(), 100)), 60),
         color="#1565c0", alpha=0.7)
axB.set_xscale("log")
for q, c, lab in [(5, "#c62828", "p5"), (50, "#000", "median"), (95, "#2e7d32", "p95")]:
    v = pct(results[base_name][1], q); axB.axvline(v, color=c, ls="--", lw=1.5, label=f"{lab} ${v:,.0f}")
axB.axvline(START, color="gray", ls=":", lw=1.5, label="start $860")
axB.set_xlabel("final capital $ after 180d (log)"); axB.set_ylabel("paths")
axB.set_title("B.  Where you land — Base regime (fat left tail = ruin risk)")
axB.legend(fontsize=8)

# Panel C: probabilities by regime
axC = fig.add_subplot(2, 2, 3)
metrics = [("P(profit)", "p_profit"), ("P(2×)", "p_2x"), ("P(loss)", "p_loss"), ("P(<½ start)", "p_half")]
x = np.arange(len(metrics)); w = 0.26
for i, (name, s) in enumerate(stats.items()):
    axC.bar(x + (i - 1) * w, [s[k] * 100 for _, k in metrics], w,
            color=REGIMES[name]["color"], label=name.split("(")[0].strip())
axC.set_xticks(x); axC.set_xticklabels([m for m, _ in metrics])
axC.set_ylabel("probability %"); axC.set_ylim(0, 100)
axC.set_title("C.  Probabilities by adverse-selection regime"); axC.legend(fontsize=8); axC.grid(alpha=0.3, axis="y")

# Panel D: final-capital spread by regime (box, log)
axD = fig.add_subplot(2, 2, 4)
data = [np.clip(results[n][1], 10, None) for n in REGIMES]
bp = axD.boxplot(data, vert=True, patch_artist=True, showfliers=False,
                 labels=[n.split("(")[0].strip() for n in REGIMES])
for patch, n in zip(bp["boxes"], REGIMES):
    patch.set_facecolor(REGIMES[n]["color"]); patch.set_alpha(0.5)
axD.axhline(START, color="k", ls=":", lw=1, label="start €800")
axD.set_yscale("log"); axD.set_ylabel("final capital $ (log)")
axD.set_title("D.  Outcome spread — the unknown dominates everything"); axD.legend(fontsize=8)
axD.grid(alpha=0.3, axis="y", which="both")

fig.tight_layout(rect=[0, 0, 1, 0.95])
out = "analysis/returns_model.png"
fig.savefig(out, dpi=110)
print(f"saved {out}")
