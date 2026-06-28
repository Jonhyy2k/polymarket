# Reward-Harvesting Strategy — design notes (thinking doc, not code)

Status: **brainstorm / for discussion.** Captures the "dynamically chase buffed,
low-competition reward markets and flee nerfed ones" idea, plus an honest take on
fees and capital. Nothing here is implemented yet — it's for thinking on during
the day and refining together later.

---

## 1. The thesis

Today's bot is a *static* LP: pick a market, rest two-sided quotes, collect
rewards (close cousin of buy-and-hold — you stand still and get paid). The idea
here is a **dynamic reward harvester**: treat the ~7,300 reward markets as a
constantly-changing yield surface and **rotate capital toward the best risk-adjusted
reward per dollar, second-by-second.**

Core loop in one line: **when a market gets *buffed* and has *low competition*,
get in fast; when it's *nerfed* or competition floods in, get out fast and
redeploy to the next best one.**

This is viable *because the rotation itself is almost free* (see §5). The edge is
**speed + selection**, which is exactly what the existing C++ stack is good at.

---

## 2. Why it's different from the current LP bot

| | Static LP (now) | Dynamic harvester (this doc) |
|---|---|---|
| Market choice | fixed (config) | continuously re-selected from the full universe |
| Reaction to buff/nerf | none | core signal — enter on buff, exit on nerf |
| Competition | ignored | primary filter (only low-comp pools) |
| Capital | sits in 1–few markets | flows to the current best set |
| Edge | being present | being *early* + *selective* |
| Speed needs | low | medium-high (first-mover on buffs) |

---

## 3. Signals we already have (all live in the dashboard API today)

Per market, from the CLOB, refreshed continuously:
- **`rate_per_day`** (pool $/day) — and its **change over time → BUFF / NERF**.
  We already track this (▲BUFFED / ▼NERFED). This is the *trigger*.
- **`market_competitiveness`** — the denominator proxy. Low = our share is big.
  This is the *filter*.
- **`rewards_max_spread`, `rewards_min_size`** — define the qualifying quote and
  the **minimum capital per market** (min_size × ~$1 for a two-sided pair).
- **DTE (days to expiry)** — safety rail. Near-expiry markets can resolve/jump;
  avoid getting caught holding inventory through resolution. Prefer DTE comfortably
  > 0; hard-avoid < 1 day unless deliberately scalping.
- **price (YES/NO)** — for capital sizing and to avoid degenerate books (near 0/1
  where one side is illiquid / can't quote on-grid).
- **our actual `earnings`** — ground truth feedback per market.

**The key derived signal we don't yet compute:** *expected reward per dollar per
hour*, risk-adjusted. Roughly:

```
score(market) ≈ (rate_per_day / 24)                      # pool per hour
              × our_qualifying_Q / (field_Q + our_Q)     # our share (uses competitiveness)
              / capital_locked(market)                   # per dollar
              × DTE_safety_factor                         # 0 near expiry, 1 when safe
              × buff_momentum                             # >1 just after a buff, <1 after a nerf
```
Rank all markets by this; deploy capital top-down until it's exhausted or the
marginal market falls below a threshold.

---

## 4. The algorithm (sketch)

```
every tick (fast — sub-second to seconds):
  1. SCAN   all reward markets; update rate, competitiveness, DTE, price.
  2. DETECT rate changes vs last snapshot → BUFF / NERF events.
  3. SCORE  every market with score() above.
  4. TARGET pick the capital-feasible top-K set (knapsack under capital + min_size
            + max-concurrent-markets + DTE-safety constraints).
  5. DIFF   compare TARGET set vs CURRENT resting set.
  6. ACT    - new high-scorers (esp. fresh BUFF + low comp): place two-sided quotes FAST.
            - dropped markets (NERF / comp spike / DTE too low / out-scored): cancel FAST,
              and if we hold inventory there, unwind it (passively if possible).
  7. GUARD  inventory caps, adverse-selection brake, kill-switch, DTE flatten.
```

Cadence: detection/scoring can run continuously; **the alpha is the latency from
"market buffed" → "our qualifying quote resting"** before competitors arrive and
compress the pool. That window is the whole game.

---

## 5. Cost model — *are fees an issue?* (short answer: no, and that's why this works)

- **CLOB maker fee: currently 0.** Resting (maker) orders that get filled pay **no
  trading fee**. The reward program *is* the maker incentive.
- **Placing / cancelling orders: off-chain, gasless, free.** An order is a signed
  EIP-712 message POSTed to the API; cancel is a DELETE. **No Polygon gas, no fee.**
  → **Rotating between markets costs essentially nothing** (just API calls + signing
  latency). This is the crucial enabler: you can re-shuffle the whole book every few
  seconds for free. A traditional "HFT has high churn cost" worry **does not apply**
  to Polymarket maker rotation.
- **Fill settlement gas:** when a maker order fills, the on-chain settlement gas is
  borne by the operator/relayer, **not charged per-fill to the maker** (to re-confirm,
  but this is the CLOB model). So a fill's real cost is **adverse selection**, not gas.
- **Deposit / withdraw:** moving money *in/out of the deposit wallet* costs a little
  Polygon gas (cents) and is occasional — not part of the rotation loop. Intra-wallet
  rotation between markets touches **no chain**.

**So the binding costs are NOT fees. They are:**
1. **Adverse selection** — your resting quote gets lifted right before the price
   moves against you. The real P&L leak. Must be measured (we have telemetry hooks).
2. **Inventory drift** — fills accumulate positions that can lose value, esp. through
   news/resolution. DTE rail + inventory caps + passive unwind address this.
3. **Capital lock-up / opportunity cost** — each qualifying two-sided pair locks
   ≈ `min_size × $1`. This is the real scarce resource.

---

## 6. Capital & feasibility — my honest take

Capital is the binding constraint, because **each reward-qualifying two-sided quote
locks ≈ `min_size × $1`** (a YES+NO pair always sums to ~$1/set). Typical min_size
20–200 ⇒ **$20–$200 locked per market.**

| Capital | Concurrent markets (at min_size ~20–50) | Verdict |
|---|---|---|
| **€20 (~$22)** | **1** | Proof-of-concept only. Rotation barely helps (you can chase the single best market, but can't diversify). What we're doing now — fine for *validating mechanics*, not for income. |
| **€200–500** | ~5–20 | **Where it starts to make sense.** Enough to diversify across low-comp pools and let buff-chasing add real alpha. Adverse-selection risk is diversified. |
| **€1k–5k** | ~20–100 | Strategy in its element — harvest the long tail of small low-comp pools that pros ignore; the buff/nerf rotation edge compounds. Watch capacity + comp compression on big pools. |
| **€10k+** | many | You become a bigger fish; share grows but competition + per-market capacity cap returns. Diminishing, still positive on the tail. |

**My view:** the idea is sound and **fee-feasible** (the free/gasless rotation is
what makes "HFT-ish" reward harvesting actually work — most venues this would be
killed by churn cost). At **€20 it's not worth the operational complexity** — run it
to learn. At **€500+ it becomes genuinely viable**, and the speed edge (first into a
buffed low-comp pool) is real money. The honest unknown that decides net return is
**adverse selection**, not fees — that's the number to measure before scaling.

---

## 7. What could kill it / risks to respect

- **Adverse selection at fills** (the #1 unknown — measure it).
- **Crowding / front-running the same buffs** — if everyone chases buffs, the edge
  is just *who's faster*. That's where C++ latency matters; if we're slow, the pool
  is already compressed by the time we arrive.
- **Resolution / news jumps** — DTE rail + don't hold inventory into events.
- **Competitiveness metric lag** — if `market_competitiveness` updates slowly, we
  may misjudge crowding. Validate it against realized share.
- **API rate limits** — scanning 7.3k markets fast + many place/cancels; need to
  respect limits and be smart about what we re-scan.
- **Polymarket changing the reward formula / fees** — the whole edge is policy-
  dependent. Monitor.

---

## 8. Role of the C++ HFT module (reuse)

- **Fast scan + score + diff** of the market universe: the C++ engine's lock-free,
  low-latency loop is ideal for "detect buff → decide" in microseconds.
- **Fast order action via the relay:** C++ decides, drives the Python connector
  (sigType-3 signing) to place/cancel — already wired (`exec_mode=relay`). Or port
  ERC-7739 TypedDataSign to C++ to remove the Python hop for full speed.
- The C++ quoter would need to switch from "bid+ask on one token" to the **cash-only
  BUY-YES + BUY-NO** structure (the open caveat already noted) to run live.
- Realistically: the *decision* speed matters more than execution µs here — the race
  is "buff happens → qualifying quote resting" vs other bots, dominated by detection
  + network RTT, not in-process µs (same geography lesson as before).

---

## 9. What to measure before committing real scale

1. **Adverse selection per fill** (the go/no-go number) — via the existing quote
   telemetry + fill capture, but on the *rotation* strategy, during volatile windows.
2. **Realized reward share vs `market_competitiveness`** — does the metric predict
   our actual slice? Calibrate `score()`.
3. **Buff→compression half-life** — after a buff, how fast does competition arrive
   and compress the pool? Defines how fast we must be and how long an edge lasts.
4. **Capacity per market** — how much size can we add before our own depth tanks our
   marginal score.
5. **Net $/day/$1k deployed** across a basket — the only honest scaling signal.

---

## 10. Parking lot / open questions

- Confirm fill-settlement gas is operator-borne (not maker) on the v2 exchange.
- Is there a lower-latency reward-change feed than polling `get_sampling_markets`?
  (A websocket or push for rate changes would be a big edge.)
- Optimal exit: passive unwind (post the other side) vs accept a tiny taker cost to
  flatten fast on a nerf — model the trade-off.
- Multi-asset reward tokens (some pools pay USDC.e vs others) — normalize value.
- Tax/accounting on many small reward credits (operational, not strategy).
- Could we *make* markets buff-attractive by being the anchor liquidity early
  (reputation / being the one the program rewards)? Probably not, but note it.

---

*Bottom line: rotation is essentially free on Polymarket (maker = no fee, place/
cancel = gasless), so a fast "chase buffs / flee nerfs in low-competition pools"
harvester is genuinely feasible — the limits are **capital** (more = more
simultaneous pools) and **adverse selection** (the unmeasured number). Worth
building out at €500+; at €20 it's a learning exercise.*
