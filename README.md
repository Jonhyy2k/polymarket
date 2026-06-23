# Polymarket CLOB v2 — Arbitrage Scanner & Shadow Liquidity-Rewards Maker (C++)

A low-latency C++ system for Polymarket's CLOB **v2** that does two things:

1. **Arbitrage scanner** — consumes the live market-data WebSocket, maintains order
   books, and detects taker/maker arbitrage across YES/NO and neg-risk groups.
   **Read-only.** Its honest verdict on real books: **taker arbitrage edge ≈ 0**
   (markets are efficient).
2. **Shadow liquidity-rewards maker** — a market-making *executor in shadow mode*
   that computes reward-qualifying two-sided quotes and runs them through a full
   order-management state machine **without sending anything** (no keys, no
   network orders, no money). Includes an **Anti-Cancel-Race (ACR)** engine
   (fast defensive cancels + inventory skew + volatility-aware quoting) with a
   dedicated cancel-sender thread. Built to evaluate the LP-rewards strategy
   before any live capital.

> **This is NOT a live trading bot.** There is no order signing, no API auth, no
> on-chain allowances, no money at risk. Everything that touches the exchange is
> read-only (market data + public REST). The maker path logs intended orders; it
> does not place them. Going live is gated on compliance clearance, key custody,
> and verification of v2 signing specs — see [Not done / going live](#not-done--going-live).

---

## Table of contents
- [Status](#status)
- [Quick start](#quick-start)
- [Architecture](#architecture)
- [The scanner](#the-scanner)
- [V2 hardening (what changed & was fixed)](#v2-hardening)
- [Geography / latency](#geography--latency)
- [Liquidity-rewards maker (shadow)](#liquidity-rewards-maker-shadow)
- [Anti-Cancel-Race (ACR)](#anti-cancel-race-acr)
- [Strategy economics (rough, honest)](#strategy-economics-rough-honest)
- [Configuration reference](#configuration-reference)
- [Tools](#tools)
- [File map](#file-map)
- [Project history & past benchmarks](#project-history--past-benchmarks)
- [Not done / going live](#not-done--going-live)
- [Roadmap](#roadmap)

---

## Status

| Component | State |
|---|---|
| Market-data feed (v2 WS) | ✅ live, schema-verified 2026-06-13 |
| Order books (bitmap best-tracking, full-depth) | ✅ hardened |
| Arbitrage scanner (taker + maker telemetry) | ✅ validated (taker edge ≈ 0) |
| Tick-size / fee (fail-closed) handling | ✅ v2-correct |
| WebSocket robustness (async timeout, reconnect) | ✅ fixed & verified |
| Feed-delivery latency instrumentation | ✅ (geography lever) |
| Liquidity-rewards quoting strategy | ✅ shadow, unit-tested |
| OMS (lifecycle, risk gate, fills) | ✅ shadow, unit-tested |
| Shadow fill simulator + net estimate | ✅ built & validated |
| **ACR engine** (at-risk detect, skew, vol-width) | ✅ shadow, unit-tested |
| **Threaded cancel-sender** (SPSC ring, pinned) | ✅ built, sub-µs hand-off |
| **MockLive gateway** (v2 order + EIP-712 digest, keyless) | ✅ built, 12 digests on live data, unit-tested |
| **Adaptive quote throttle** (ACR-aware) | ✅ built, unit-tested |
| **Quote telemetry capture** (adverse-selection groundwork) | ✅ built (CSV on logger thread) |
| **pUSD allowance sim** (pre-trade gate) | ✅ built, unit-tested |
| v2 EIP-712 spec | ⚠️ verified-from-docs, **UNVERIFIED on-chain** (1 open field) |
| **Live execution (signing/auth/allowances)** | ❌ **not built — gated** |
| Fill-probability model (trustworthy net) | ❌ needs the telemetry capture above over hours/days |

Latency (laptop, in-process `t0→t3`): parse p50 ~1.2µs, book p50 ~0.3µs, arb
p50 ~0.6µs, e2e p50 ~3.5µs; OMS→sender hand-off p50 0.36µs / p99 2.2µs.
**The matching engine is in London (AWS eu-west-2)**; feed delivery and order
RTT are network-bound (see [Geography](#geography--latency)) and dominate the
in-process pipeline by ~4 orders of magnitude.

---

## Quick start

### Build
```bash
# deps: gcc/clang (C++17), cmake>=3.16, OpenSSL, Boost (header-only; system or conda)
# one-time: fetch simdjson single-header into deps/
mkdir -p deps && cd deps
curl -sL https://raw.githubusercontent.com/simdjson/simdjson/master/singleheader/simdjson.h  -o simdjson.h
curl -sL https://raw.githubusercontent.com/simdjson/simdjson/master/singleheader/simdjson.cpp -o simdjson.cpp
cd ..

mkdir -p build && cd build
cmake ..            # auto-detects system Boost, else conda; system OpenSSL
cmake --build . -j4 # builds arb_detector + test_executor
```
On a sudo-less dev box use anaconda for Boost/OpenSSL; `deploy/setup_ec2.sh`
provisions a fresh EC2 box (installs deps, downloads simdjson, Release build).

### Run the scanner
```bash
# generate a live crypto config from gamma (needs network)
python3 tools/build_live_config.py config.live.json 16
./build/arb_detector --config config.live.json
```

### Run the shadow rewards maker
```bash
# rewards live on sports/politics/event markets, NOT crypto — build from /sampling-markets
# (config.rewards.json is a checked-in example; regenerate as markets rotate)
./build/arb_detector --config config.rewards.json     # shadow_executor_enabled=true
```

### Tests
```bash
./build/test_executor   # 35 deterministic checks: scoring, Qmin, quoting, OMS, risk, fills, ACR
./build/live_test       # 29 deterministic checks: Keccak/EIP-712, v2 mapping, MockLive, throttle, allowance
```

---

## Architecture

Lock-free, pinned-thread pipeline. Threads communicate via SPSC ring buffers; no
locks on the hot path.

```
 market WS (/market, TLS)                    CLOB/gamma REST (startup, read-only)
        │                                     reward + market metadata
   [receiver thread]  ── stamps recv steady+wall clock
        │ SPSC (MessageSlot ring)
        ▼
   [parser thread]  ── simdjson in-place parse → book update → arb check
        │                └─ shadow rewards executor + ACR (post-arb, off timing path)
        │                      │ SPSC (OrderCommand ring)        │ SPSC (metrics/opp/replay)
        │                      ▼                                 ▼
        │              [cancel-sender thread]            [logger thread]
        │               ShadowGateway (logs;              CSV + periodic summary
        │               LiveGateway later)                (latency, edges, P&L, feed)
```

- **Zero-copy ingest**: raw WS frame → `memcpy` into a pre-padded simdjson buffer.
- **Order book**: dense `size_by_price[1001]` array with an epoch trick for O(1)
  clears, plus a **1024-bit occupancy bitmap** so best-bid/ask is a few word ops
  (`clz`/`ctz`) instead of a 1001-slot linear scan.
- **ACR detection runs inline** on the parser thread (no thread hop on the
  critical path); the cancel/create **send is offloaded** to a pinned cancel-sender
  thread via an SPSC `OrderCommand` ring, so the hot path never blocks on I/O.
  OMS order state stays single-threaded (parser-owned) — no locks.
- **Threads pinned** to configurable CPUs; optional RT priority, mlock, stack
  prefault.

---

## The scanner

For every contract it checks, fee-aware:
- **Taker** `BUY_BOTH` (buy YES ask + NO ask < $1) / `SELL_BOTH` (sell both bids > $1).
- **Neg-risk group** arbitrage across exhaustive partitions (auto-detected).
- **Maker** variants (post one tick inside, take the other leg) — **telemetry only**,
  excluded from paper P&L because they assume fills you don't control.

**Result on real books:** every taker edge is negative (best ~ −20 bps net) →
**Paper P&L (TAKER) = $0.00.** That is the correct *null* result — the market is
efficient, not the engine failing. Maker "edge" shows up only on a separate
excluded line and is hypothesis-grade until a fill model exists.

---

## V2 hardening

Polymarket cut over to CLOB v2 (new exchange contracts, pUSD collateral,
taker-only fees + maker rebate). The scanner was re-verified against a live frame
capture and these issues were fixed (all validated):

- **Best-bid truncation (severe):** book `bids` arrive **ascending** (best last)
  and can exceed 50 levels; the old parse loop capped at `MAX_LEVELS` and dropped
  the best bid. Now the dense ladder ingests **every** level (display array only
  is capped). Verified on a live 65-level book.
- **Tick size:** ticks are **per-market** (0.001 mid-range, 0.01 near 0.50).
  Now tracked per book (seeded from gamma `orderPriceMinTickSize`, updated from
  the `book` event's `tick_size`, plus a `tick_size_change` handler). Maker
  inside-prices step by the real tick and refuse to quote off-grid.
- **Fees fail-CLOSED:** a fee-schedule parse miss no longer collapses to "zero
  fees" (which manufactures edge). Unknown ⇒ charge the configured fallback.
  *(The v2 fee formula itself is still unvalidated — captured `fee_rate_bps`
  were 0 maker fills.)*
- **WebSocket timeout was dead code:** Beast `tcp_stream` timeouts apply only to
  async ops; the old synchronous read blocked forever on quiet markets (pings /
  stale-reconnect / shutdown all dead). Switched to `async_read` + `expires_after`
  — verified `timeouts` now fire (15 in a quiet-market test); cooperative shutdown
  ~0.17s; zombie-connect-on-shutdown fixed.
- **Performance:** occupancy-bitmap best-tracking dropped book-update p99 from
  ~6.7µs to ~2.3µs.
- **Metrics:** `arb_us`/`e2e_us` no longer contaminated by replay-emission
  (timing stamped before logging); opp-CSV `parse_us` renamed `recv_parse_us`
  (it spans receipt→parse-done, includes queue wait).

Live v2 market-channel schema is documented in
`POLYMARKET_ARB_DETECTOR_PLAN.md` and the capture tool below.

---

## Geography / latency

The dominant lever is **not** in-process µs — it's the network. In-process is
single-digit µs; the network is tens of milliseconds, ~4 orders larger.

**The matching engine is in London (AWS eu-west-2).** Established by a
clock-*independent* warm round-trip from Lisbon to the CLOB origin = **47.6 ms**
(≈ London 35 ms RTT + ~12 ms server processing; us-east-1 measures 125 ms and is
ruled out), corroborated by public infra write-ups. Settlement is on Polygon via
the CTF Exchange contract; the **public** CLOB API (REST + market WS) is
**Cloudflare-fronted** — you connect to the nearest CF edge (check the `cf-ray`
response header for the PoP code, e.g. `-AMS`/`-LHR`), and CF relays to the London
origin. So RTT ≈ [you → CF edge] + [CF edge → London origin → back]: the first leg
you control by location, the second is shared.

> ✅ **Correction (2026-06, supersedes the earlier "no raw sub-Cloudflare access for
> anyone"):** Polymarket's CLOB docs confirm **direct colocation in `eu-west-2` is
> available** to anyone who completes their **KYC/KYB form** — a *direct, sub-
> Cloudflare* line to the engine, "the lowest feasible latency to Polymarket's
> primary servers." That is how the fastest players reach sub-ms: they **colocate**,
> they don't fight the Cloudflare path from afar. It is the *sanctioned institutional*
> route (in the geo-restricted region, behind KYB), so it sits **downstream of
> compliance gate #1**, not around it.

> ⚠️ **Earlier correction (kept for the record):** notes in this repo's history said
> "us-east". That came from the one-way `Feed deliv (ms)` floor (~44 ms), inflated by
> ~25 ms of **laptop NTP clock skew**. The clock-independent round-trip says
> **London**. Trust round-trip, not skewed one-way.

**Maker lifecycle latency, from the Lisbon laptop (measured RTT):**

| step | Lisbon (now) | Ireland eu-west-1 | London eu-west-2 (banned to run) |
|---|---|---|---|
| see a book update (engine → you) | ~17–18 ms | ~5 ms | ~1 ms |
| post a quote (decision → resting at engine) | ~18–22 ms | ~6–8 ms | ~2 ms |
| learn of a fill (engine → you) | ~17–18 ms | ~5 ms | ~1 ms |
| **tick-to-react** (move at engine → your cancel back at engine) | **~35 ms** | **~10 ms** | ~2 ms |

In-process work (~5 µs host total) is buried inside each row — it rounds to zero.

**Deployment (non-colocated):** the engine is in London (eu-west-2, geo-restricted
to run from). Polymarket's docs designate **`eu-west-1` (Ireland) as the closest
*unrestricted* region** — and because the engine is in AWS, an instance in AWS
eu-west-1 can ride AWS's dedicated **inter-region backbone** to eu-west-2, which
plausibly beats a geographically-closer **Amsterdam** that crosses public internet +
Cloudflare (community reports claim Dublin 0–1 ms to the backend; unverified VPS
marketing). This **revises an earlier steer toward Amsterdam**: Amsterdam's logic
holds for the public CF endpoint, but for an AWS deploy Ireland is the intended path
and likely wins. **Measure before committing** — A/B candidate regions on the
`cf-ray` PoP + warm **TCP-connect RTT** (not the skew-prone one-way feed number),
run **chrony/PTP**, trust round-trip.

**Implication:** the decisive lever is **location** (Lisbon → Ireland, or colo in
eu-west-2), not host micro-optimization. Kernel bypass / io_uring / busy-poll / NIC
tuning save *microseconds* on the host path — noise against the ms network **while
remote**. The AWS "tick-to-trade" stack (cluster placement groups, ENA, DPDK/XDP,
bare-metal — see AWS Web3 blog Pts 1–2) optimizes traffic *between your own EC2
instances* and **only becomes material once you colocate**, when the network shrinks
to ms/µs and host time finally dominates. It is a **package with colocation**, not a
remote-client win. For a **rewards-LP** (earns by *standing* in the book, not racing)
colocation + that µs-stack is likely **overkill** — the cancel-race isn't where the
money is; the unmeasured **adverse-selection** number is.

---

## Liquidity-rewards maker (shadow)

Targets Polymarket's **LP rewards program** (`docs/market-makers/liquidity-rewards`),
verified live 2026-06-13. You earn USDC daily for **standing in the book** near
mid — fills are almost a side effect, which suits a latency-disadvantaged operator.

**Scoring (verified):** a qualifying order is within `max_spread` (cents) of mid
and ≥ `min_size`. Per-order score `S(v,s) = ((v−s)/v)² · size` (v = max_spread,
s = distance from mid). Per side `Q_one`/`Q_two`; combine:
`Qmin = max(min(Q1,Q2), max(Q1/c, Q2/c))` with `c=3.0` for mid ∈ [0.10, 0.90]
(single-sided scores at 1/3); outside that range it must be two-sided. Sampled
each minute; 7-day epoch; payout = `your Q_epoch / Σ(all makers) × pool`, paid
daily in USDC. Read config from `GET clob.polymarket.com/markets/{cid}` →
`rewards{rates,min_size,max_spread}` (`rates:null` ⇒ not currently emitting).

**Components:**
- `rewards.{hpp,cpp}` — pure quoter: two-sided, on-grid, tight as the tick allows
  within `max_spread`, ≥ `min_size`; computes estimated Qmin.
- `oms.{hpp,cpp}` — order lifecycle state machine, client IDs, desired-vs-live
  reconcile (idempotent; cancel+recreate on change), **inline risk gate** (gross
  notional, position cap, max open orders, kill switch), fills→position. The OMS
  talks to a `ThreadedGateway` that **enqueues** create/cancel commands to the
  cancel-sender thread (state stays single-threaded). `ShadowGateway` (on the
  sender side) logs intended actions; `IExecGateway` is the seam for a future
  `LiveGateway`.
- `tools/fill_sim.py` — live-replays reward markets, posts the quotes, models
  fills from public trades + queue position, marks adverse selection at +30s, and
  estimates net = reward + inventory mark.

**Important real-world finding:** the rewards program lives on **sports / politics
/ event** markets, **not** the crypto up/down markets (those report `rates:null`).
So the rewards maker targets a different universe than the scanner.

---

## Anti-Cancel-Race (ACR)

A maker quoting near mid gets *picked off* when the market moves: a taker lifts
your now-stale order right before the price moves against you (adverse selection).
ACR is the defensive layer that **cancels at-risk quotes fast** and quotes more
conservatively when it's dangerous. `src/acr.hpp` (header-only, inlines on the hot
path):

- **At-risk detection** — a resting quote is flagged when it has reached/crossed
  the mid, *or* the mid has drifted ≥ N ticks against it since it was placed →
  urgent cancel. Runs **inline on the parser thread** (no thread hop).
- **Inventory skew** — when net-long, shift both quotes down (ask cheaper = sell
  easier, bid cheaper = buy harder) to mean-revert toward flat.
- **Volatility widening** — an EWMA of `|Δmid|` pushes quotes further from mid when
  the market is moving fast, so a jump is less likely to fill you.

**Threading:** detection is inline (lowest latency); the cancel **send** is
offloaded to the pinned cancel-sender thread via an SPSC `OrderCommand` ring.
Measured in-process hand-off (OMS decide → sender send): **p50 0.36 µs / p99
2.2 µs**. Reaction (book recv → at-risk detect) is sub-µs.

**Honest caveats:**
- The in-process µs is *not* the cancel time. The real cancel = detect + hand-off
  + sign + **network RTT to London** + engine processing. From Lisbon, tick-to-react
  is **~35 ms** (network-bound) → ACR can't win the race from a laptop. From
  **eu-west-1 it's ~10 ms** and the race becomes winnable. ACR's value is gated by
  *location* (see [Geography](#geography--latency)).
- ACR's urgent cancel matters most when re-quoting is **throttled** (live rate
  limits / queue-priority preservation). With re-quote-every-tick (the shadow
  default) the base reconcile re-centers before drift builds, so at-risk rarely
  fires — the unit tests prove detection works on real moves.

35 deterministic checks in `test_executor` cover scoring/Qmin/quoting/OMS/risk/
fills + ACR (vol EWMA, at-risk cross & drift, skew, vol-widen). A second binary
`live_test` adds 29 checks for the (mock) live path (Keccak/EIP-712, v2 mapping,
MockLiveGateway, throttle, allowance) — see below.

---

## Execution modes (Shadow / MockLive / Live)

The OMS, reconcile, risk gate, ACR, inventory and throttle are all **mode-agnostic**
— they only ever see an `IExecGateway`. `exec_mode` selects which gateway the
cancel-sender thread uses; nothing upstream changes. This is the clean seam to
the live path.

| mode | what the send side does | keys? | network? |
|---|---|---|---|
| `shadow` (default) | logs the intended create/cancel | no | no |
| `mocklive` | **builds the real v2 order + computes the full EIP-712 digest**, counts it, audits the quote — then stops | **no** | no |
| `live` | real signer + CLOB POST — **not built, gated**; falls back to shadow with a warning | (gated) | (gated) |

**MockLive** is the middle rung that de-risks going live without custody. On every
create it maps the resting quote → the v2 `Order` struct → the 32-byte EIP-712
typed-data hash an EOA *would* sign, exercising the whole serialize/hash path so
the format is validated and the cost measured. It never signs and never sends.
A live 35 s run computed **12 EIP-712 digests on real reward-market orders** with
zero off-grid/too-wide near-misses.

### v2 signing spec (verified-from-docs, **UNVERIFIED on-chain**)

Captured 2026-06 from the Polymarket v2 migration docs + community references.
`src/eip712.hpp` computes **real** Keccak-256 EIP-712 digests from these — but they
are **not yet reconciled against the deployed contract**, so a digest is for path
validation only, **never** a live signature, until checked. `MockLiveGateway::describe()`
prints the derived `ORDER_TYPEHASH` + domain separators at startup for exactly that
reconciliation.

| field | value | confidence |
|---|---|---|
| Order struct | `salt, maker, signer, tokenId, makerAmount, takerAmount, side(u8), signatureType(u8), timestamp, metadata(b32), builder(b32)` | high (3 sources) |
| `ORDER_TYPEHASH` (derived) | `0xbb86318a…33818589` | computed |
| domain name / version / chainId | `Polymarket CTF Exchange` / `"2"` / `137` | high |
| Exchange (standard) | `0xE111180000d2663C0091e4f400237545B87B996B` | high |
| Exchange (neg-risk) | `0xe2222d279d744050d28e00520010520000310F59` | high |
| **neg-risk domain `name`** | defaulted to `Polymarket CTF Exchange` | ⚠️ **open** — migration doc says this, a cheatsheet says `Polymarket Neg Risk CTF Exchange`; flip `kNegRiskName` if chain says so |
| signatureType | `0 EOA, 1 POLY_PROXY, 2 POLY_GNOSIS_SAFE` | high |
| collateral | pUSD, 6 dp (allowance-sim only; not in the hash) | med |

v1→v2 dropped `taker/expiration/nonce/feeRateBps` and added `timestamp/metadata/builder`,
and bumped the domain version `"1"→"2"`. Building the v1 struct from memory would
have hashed the wrong order — hence the doc-verification gate.

### Adaptive quote throttle

Re-quoting every tick is fine in shadow but live it shreds queue priority and burns
rate limits. `src/throttle.hpp` holds the resting quote between moves and only acts
when it must — **ACR at-risk, or a side being added/removed, always bypass**. Cadence
adapts: **calm → lengthen** (keep your queue spot), **volatile → shorten** (re-center
fast). This is the corrected logic (calm slows, volatile speeds — the inverse of the
common first sketch), and it's what makes ACR matter (ACR's urgent cancel is only
meaningful once re-quoting is throttled). In the live run the throttle held **1120**
reconciles against 12 acted-on changes.

### pUSD allowance + telemetry

- **Allowance sim** (`risk_pusd_allowance_usd`): gross resting notional may not exceed
  the approved pUSD — a hard wallet constraint checked pre-submit, distinct from the
  strategy gross cap. First-order proxy (doesn't yet model fills consuming collateral).
- **Quote telemetry** (`quote_telemetry_enabled` → `quote_telemetry.csv`): per-reconcile
  mid/spread/quote/dist/Qmin/vol/ACR-risk/eligible, drained on the logger thread. This
  is **Roadmap #1** — the raw capture to finally measure adverse selection / net.
- **Near-miss-live audit** (`near_miss_live.csv`, mocklive): logs any quote a venue
  would reject — off-grid, beyond `max_spread`, or self-crossed.

`live_test` covers all of the above deterministically (Keccak vectors, typehash/domain
stability as a regression guard, digest determinism + field-sensitivity, gateway
mode-parity, allowance, throttle, validator).

---

## Strategy economics (rough, honest)

Measured from live qualifying book depth + the fill simulator (2026-06-14).
**All figures are GROSS reward (a ceiling), snapshots, and net is unproven.**

- **Fills are rare** (~1 trade per token per ~7 min; thousands of price-changes per
  trade). A maker one tick inside is seldom hit ⇒ low adverse selection ⇒ the
  strategy is **reward-dominated by design**. In a 180s sim: 7 trades → 0 fills →
  net ≈ gross (but that's too small a sample to *prove* low adverse selection).
- **Capital scaling (greedy allocation, 60%-share cap):**

  | capital | gross $/day | $/mo | %/day |
  |---|---|---|---|
  | ~$860 (€800) | ~$141 | ~$4,240 | 16% |
  | ~$3,000 | ~$357 | ~$10,700 | 12% |
  | ~$10,000 | ~$692 | ~$20,800 | 7% |
  | ~$30,000 | ~$1,040 | ~$31,200 | 3.5% |

- **Strongly sublinear / capacity-bound:** 35× capital → ~7× reward. Thin markets
  pay the most per dollar but saturate fast; big pools (World Cup, $3k/day) are
  already saturated and pay ~$0. **This is a small-capital niche, not scalable.**
- **⚠️ Double-digit %/day gross is a red flag, not a forecast.** It is gross of the
  unmeasured adverse-selection cost, concentrated in 2–3 thin markets, and a
  snapshot that competition compresses. Treat it as a ceiling you will not hit.
  The real net needs hours/days of capture (or tiny live posting) to measure
  adverse selection.

**Capital model:** a two-sided quote on a binary market locks ≈ `size × $1` of
USDC (YES bid at `p` + NO bid at `1−p`, since YES+NO=1, so you market-make both
sides of YES with USDC only — no need to pre-hold shares).

---

## Configuration reference

JSON (see `config.strategy.json`). Selected keys:

| key | meaning |
|---|---|
| `taker_fee_bps` | fallback taker fee when metadata is unknown (fail-closed) |
| `min_edge_threshold_bps` | min net edge to emit a signal |
| `maker_arb_enabled` | compute maker telemetry signals |
| `enable_group_arbitrage`, `auto_detect_exhaustive_groups` | neg-risk groups |
| `fetch_market_metadata` | pull fees/tick/neg-risk from gamma at startup |
| `receiver_cpu` / `parser_cpu` / `logger_cpu` | thread pinning |
| `*_priority`, `lock_memory`, `prefault_stack_kb` | RT tuning |
| **`shadow_executor_enabled`** | run the shadow rewards maker |
| **`shadow_executor_verbose`** | log every intended create/cancel |
| **`reward_quote_size`** | shares/side (0 ⇒ each market's `min_size`) |
| **`reward_target_offset_thou`** | distance inside mid (0 ⇒ tightest = 1 tick) |
| **`risk_max_gross_notional_usd`** | OMS pre-trade gross cap |
| **`risk_max_position_shares`** | OMS per-token position cap |
| **`risk_max_open_orders_total`** | OMS open-order cap |
| **`acr_enabled`** | run the Anti-Cancel-Race engine |
| **`acr_stale_drift_ticks`** | cancel when mid drifts this many ticks against a quote |
| **`acr_inv_skew_per_share_thou`** | quote shift per share of inventory (flattening) |
| **`acr_vol_widen_k`** | widen each side by `k × mid-vol EWMA` |
| **`command_queue_capacity`** | OMS → cancel-sender ring size |
| **`sender_cpu`** / **`sender_priority`** | cancel-sender pinning (−1 = unpinned) / SCHED_FIFO priority |
| **`exec_mode`** | `shadow` (log), `mocklive` (build v2 order + EIP-712 digest, no key/send), `live` (gated) |
| **`live_maker_address`** / **`live_signer_address`** / **`live_signature_type`** | (mock) signer identity; placeholders until custody |
| **`risk_pusd_allowance_usd`** | simulated pUSD allowance cap (0 = unlimited) |
| **`quote_throttle_enabled`** / **`_min_ms`** / **`_max_ms`** / **`_vol_hot_thou`** | adaptive throttle on/off, cadence bounds, volatility threshold |
| **`quote_telemetry_enabled`** / **`_log_file`** / **`_queue_capacity`** | per-reconcile telemetry capture → CSV |
| **`near_miss_live_log_file`** | mocklive off-grid/too-wide/crossed audit CSV |
| **`ws_tls13_enabled`** / **`ws_permessage_deflate`** | transport A/B (TLS 1.3 negotiate; WS compression) |

Example configs in the repo: `config.live.json` (crypto scanner),
`config.rewards.json` (reward-active maker), `config.acr.json` (maker + ACR),
`config.mocklive.json` (maker + ACR + MockLive EIP-712 + throttle + telemetry),
`config.quiet.json` (WS timeout test), `config.strategy.json` (base settings).

---

## Tools

| tool | purpose |
|---|---|
| `tools/build_live_config.py` | build a crypto scanner config from gamma (tag_id=21) |
| `tools/ws_schema_dump.py` | capture raw v2 WS frames → `ws_raw_frames.jsonl` |
| `tools/fill_sim.py` | shadow fill simulator + net-PnL estimate (reward markets) |
| `tools/hedge_arb_test.py` | live paper test for buy/sell-both free-arb (proves no taker free lunch) |

All gamma/CLOB calls need a `User-Agent` header (else 403).
To build a rewards config, select reward-active markets from
`GET clob.polymarket.com/sampling-markets` (the crypto markets have no rewards).

---

## File map

```
src/
  main.cpp            pipeline wiring, config, threads, dispatch, shadow executor hook
  websocket.{hpp,cpp} TLS WS client (async read + timeout, reconnect)
  parser.hpp          simdjson event parsing (book/price_change/best_bid_ask/trade/tick)
  orderbook.{hpp,cpp} dense ladder + occupancy bitmap best-tracking
  arbitrage.{hpp,cpp} taker/maker/group arb checks, fail-closed fees
  rewards.{hpp,cpp}   liquidity-rewards quoting strategy (scoring + quoter)   [maker]
  oms.{hpp,cpp}       OMS state machine + risk gate + ThreadedGateway/ShadowGateway + ExecMode [maker]
  acr.hpp             Anti-Cancel-Race engine (header-only: at-risk, skew, vol-width) [maker]
  throttle.hpp        adaptive quote throttle (calm/volatile cadence; ACR bypass) [maker]
  eip712.hpp          Keccak-256 (OpenSSL) + v2 EIP-712 digest (UNVERIFIED on-chain) [mocklive]
  live_order.hpp      v2 LiveOrderPayload + reward-quote→order mapping + quote validator [mocklive]
  mock_live_gateway.* MockLiveGateway (build order + digest, no key/send) + NearMissLiveLog [mocklive]
  market_metadata.*   gamma fee/tick fetch + ClobRewardsClient (reward config)
  logger.{hpp,cpp}    CSV + summary (latency incl. feed-delivery, edges, P&L)
  pipeline.hpp        SPSC ring, MessageSlot, MetricsEvent, OrderCommand, QuoteTelemetryEvent
  types.hpp           Price/Size/book/contract/config structs, clocks
tests/
  test_executor.cpp   35 deterministic checks (strategy + OMS + ACR)
  live_test.cpp       29 deterministic checks (Keccak/EIP-712 + v2 mapping + MockLive + throttle + allowance)
tools/                config builders, schema dump, fill simulator, hedge-arb test
deploy/setup_ec2.sh   provision a fresh EC2 box
```

Repository: <https://github.com/Jonhyy2k/polymarket> (local dir `polymarket-clob`).

---

## Project history & past benchmarks

The project began as a pure arbitrage detector (see `POLYMARKET_ARB_DETECTOR_PLAN.md`)
built in 7 phases — connection test → simdjson parser + orderbook → multi-token
(YES/NO) → arb detector → paper-trade logger → multi-contract → reconnection.
Original success criteria included **"< 10µs T0→T3"** and **"zero heap allocations
on the hot path."** `instructions.txt` is the situation brief from the first
hardening pass; `outputs.txt` holds the original benchmark logs.

### Original benchmarks (April 2026, pre-v2-hardening) — for the record

Three 5-second runs against the old `config.strategy.json` (Fed-April + football
markets). Latency then vs now:

| stage (p50) | April 2026 (unhardened, 48 KB snapshot frames) | now (hardened, this session) |
|---|---|---|
| Queue wait | 395.8 µs | 0.9 µs |
| Parse (frame) | 196.4 µs | 1.2 µs |
| Book upd | 178.3 µs | 0.3 µs |
| Arb batch | 25.1 µs | 0.6 µs |
| **End-to-end (t0→t3)** | **~411 µs** | **~3.5 µs** |
| Msg size | up to 48,871 B | ~612 B |

The ~100× improvement comes from the bitmap best-tracking, the metrics-contamination
fix, and — largely — that the old runs were re-parsing giant 48 KB initial-dump
snapshots of dead markets under queue backpressure. The original **< 10µs T0→T3
goal is now met in-process** (network delivery, ~44–90 ms, is the real latency).

### ⚠️ The "$9,833 P&L" was a bug, not performance

Those April runs reported **"Cumulative P&L: $9,833.11 / $0.00 / $10,181.56"** across
the three 5-second benchmarks, with absurd group edges (e.g. −8,670 bps). **This was
not profit — it was three compounding bugs**, all since fixed:

1. **Every configured market was resolved/dead** (Fed April decision, April 10–11
   football) → nonsensical books and edges.
2. **Maker signals were counted toward paper P&L** as if they were taker-executable
   fills (they are hypotheses, not fills).
3. **Size-unit confusion** (`size_usd` vs `size_shares`) inflated notional.

After the fixes — maker excluded from paper P&L, a live market config, and the
v2 hardening — the **true taker paper P&L is $0.00** (the correct null on efficient
books). **Disregard any P&L figure in `outputs.txt`.**

### Related documents
- `POLYMARKET_ARB_DETECTOR_PLAN.md` — original build plan, v2 WS protocol reference,
  arbitrage formulas (BUY_BOTH/SELL_BOTH/group), price representation, build phases.
- `instructions.txt` — first-pass situation brief and bug hunt.
- `outputs.txt` — original benchmark logs (**P&L numbers are the bug described above**).

---

## Not done / going live

The live execution path is **deliberately unbuilt** — it is the irreversible,
keys-and-money line. Building it requires, in order:

1. **Compliance clearance** (BNP PAD) + a jurisdiction where Polymarket permits the
   account/IP. This gates everything.
2. **Verify v2 signing specs** against current docs/on-chain: Exchange contract
   address, chainId, **pUSD** collateral, EIP-712 Order struct, L2 auth header
   scheme. Do not build from memory. *(Partly done: spec verified-from-docs and
   encoded in `src/eip712.hpp`; MockLive computes real digests from it. Still
   needs on-chain reconciliation — one open field, the neg-risk domain `name` —
   before any digest is trusted for a signature. `MockLiveGateway::describe()`
   prints the values to reconcile.)*
3. **Key custody** decision (HSM/KMS/hot-wallet blast radius).
4. `LiveGateway` implementing `IExecGateway`: EIP-712 signer (the digest is
   already computed by `eip712.hpp` — add the ECDSA sign over it), L2-authed REST
   POST/DELETE, the authenticated **user WS** for real fills, on-chain allowance
   setup. *(The seam + order serialization + digest exist via MockLive; the
   remaining delta is the key, the signature, and the network calls.)*
5. **Fill-probability / adverse-selection model** from a long capture or tiny live
   posting — the only way to turn "gross ceiling" into a trustworthy net. *(The
   capture is now built — `quote_telemetry.csv`; run it for hours/days.)*

Until then: scanner is read-only; maker is shadow/mocklive-only (no key, no send).

---

## Roadmap

1. **Run the quote-telemetry capture for hours/days** (now built —
   `quote_telemetry.csv`) and fold it into `fill_sim.py` offline replay → a
   trustworthy net-of-adverse-selection number. *(Zero risk; do first.)* This is
   **the** number that decides whether the true-LP shift makes money.
2. **Pick the region empirically, then deploy.** Polymarket designates **eu-west-1
   (Ireland) as the closest *unrestricted* region** (engine in eu-west-2, geo-
   restricted). On AWS, eu-west-1 likely beats Amsterdam via AWS's backbone to
   eu-west-2 — but A/B candidates on `cf-ray` PoP + warm TCP-connect RTT before
   committing (run chrony/PTP; trust round-trip, not one-way). The bigger lever is
   **KYB colocation in eu-west-2** (sub-ms, direct, sub-Cloudflare) — gated by
   compliance, and likely overkill for a rewards-LP.
3. **Reconcile the v2 EIP-712 spec on-chain** (resolve the neg-risk domain `name`;
   confirm `ORDER_TYPEHASH` + domain separators against the deployed Exchange).
4. Validate the v2 fee formula against a non-zero `last_trade_price.fee_rate_bps`.
5. Live execution milestone (only after clearance) — add ECDSA signing over the
   MockLive digest + L2 auth + allowances; see above.
6. Host µs-stack (kernel bypass / busy-poll / CPU isolation / ENA / placement
   groups) — **only material if you colocate**; noise vs the ms network while
   remote. Defer until colocated and only if profiling says host time matters.

### Runbook — pinning the sender (once on a low-latency box)
At boot: `isolcpus=3 nohz_full=3 rcu_nocbs=3` (reserve a full **physical** core, not
a hyperthread sibling of the parser). In config: `"sender_cpu": 3, "sender_priority":
80`. The binary applies CPU affinity + SCHED_FIFO via `apply_thread_runtime_tuning`.
This is a p99-jitter (tail) play, not a median one — the network ms still dominates
until you colocate.

---

*Scanner is read-only; the maker is shadow/mocklive-only — MockLive builds real v2
orders and EIP-712 digests but holds no key, signs nothing, and sends nothing. No
keys, no live orders, no money at risk. The v2 signing spec is verified-from-docs
but UNVERIFIED on-chain. Reward/PnL figures are gross, snapshot, order-of-magnitude
estimates with unproven net — not financial advice or a performance promise.*
