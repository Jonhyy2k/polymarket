# Polymarket CLOB v2 — Arbitrage Scanner & Shadow Liquidity-Rewards Maker (C++)

A low-latency C++ system for Polymarket's CLOB **v2** that does two things:

1. **Arbitrage scanner** — consumes the live market-data WebSocket, maintains order
   books, and detects taker/maker arbitrage across YES/NO and neg-risk groups.
   **Read-only.** Its honest verdict on real books: **taker arbitrage edge ≈ 0**
   (markets are efficient).
2. **Shadow liquidity-rewards maker** — a market-making *executor in shadow mode*
   that computes reward-qualifying two-sided quotes and runs them through a full
   order-management state machine **without sending anything** (no keys, no
   network orders, no money). Built to evaluate the LP-rewards strategy before
   any live capital.

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
| **Live execution (signing/auth/allowances)** | ❌ **not built — gated** |
| Fill-probability model (trustworthy net) | ❌ needs long capture / live data |

Latency (laptop, in-process `t0→t3`): parse p50 ~1.2µs, book p50 ~0.3µs, arb
p50 ~0.6µs, e2e p50 ~3.5µs. **Feed delivery from Lisbon: ~44ms floor / ~90ms p50**
— the network dominates the in-process pipeline by ~4 orders of magnitude.

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
./build/test_executor      # 25 deterministic checks: scoring, Qmin, quoting, OMS, risk, fills
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
   [parser thread]  ── simdjson in-place parse → order book update → arb check
        │                └─ shadow rewards executor (post-arb, off timing path)
        │ SPSC (metrics / opportunities / replay rings)
        ▼
   [logger thread]  ── CSV + periodic summary (latency, edges, P&L, feed delivery)
```

- **Zero-copy ingest**: raw WS frame → `memcpy` into a pre-padded simdjson buffer.
- **Order book**: dense `size_by_price[1001]` array with an epoch trick for O(1)
  clears, plus a **1024-bit occupancy bitmap** so best-bid/ask is a few word ops
  (`clz`/`ctz`) instead of a 1001-slot linear scan.
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

The dominant lever is **not** in-process µs — it's the network. The binary stamps
`CLOCK_REALTIME` at WS receipt and reports `Feed deliv (ms)` = `recv_wall −
newest event.timestamp`.

- **Lisbon residential baseline: floor ~44ms, p50 ~90ms.** With a ~2ms edge RTT,
  that floor means Polymarket's origin is **transatlantic (≈ us-east-1)**.
- In-process e2e is ~3.5µs → feed delivery is **~40,000× larger.**
- **The trap:** the only location that wins (us-east-1, ~single-digit ms) is where
  Polymarket geo-blocks users; UK (eu-west-2) is also blocked. eu-west-1 (Ireland)
  mainly removes residential *jitter* (p50 → ~45ms), not the ~35–40ms transatlantic
  *floor*. Run the same binary per region to A/B; keep clocks chrony/NTP-synced
  (the delta includes skew).

**Implication for strategy:** a transatlantic operator cannot win latency races
(taker arb). The viable game is the **maker/rewards** business, where being 40ms
vs 5ms from the feed matters far less than inventory and fill modeling.

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
  notional, position cap, max open orders, kill switch), fills→position.
  `ShadowGateway` logs intended actions; `IExecGateway` is the seam for a future
  `LiveGateway`.
- `tools/fill_sim.py` — live-replays reward markets, posts the quotes, models
  fills from public trades + queue position, marks adverse selection at +30s, and
  estimates net = reward + inventory mark.

**Important real-world finding:** the rewards program lives on **sports / politics
/ event** markets, **not** the crypto up/down markets (those report `rates:null`).
So the rewards maker targets a different universe than the scanner.

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

Example configs in the repo: `config.live.json` (crypto scanner),
`config.rewards.json` (reward-active maker), `config.quiet.json` (WS timeout test),
`config.strategy.json` (base settings).

---

## Tools

| tool | purpose |
|---|---|
| `tools/build_live_config.py` | build a crypto scanner config from gamma (tag_id=21) |
| `tools/ws_schema_dump.py` | capture raw v2 WS frames → `ws_raw_frames.jsonl` |
| `tools/fill_sim.py` | shadow fill simulator + net-PnL estimate (reward markets) |

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
  oms.{hpp,cpp}       order management state machine + risk gate + shadow gateway [maker]
  market_metadata.*   gamma fee/tick fetch + ClobRewardsClient (reward config)
  logger.{hpp,cpp}    CSV + summary (latency incl. feed-delivery, edges, P&L)
  pipeline.hpp        SPSC ring, MessageSlot, MetricsEvent
  types.hpp           Price/Size/book/contract/config structs, clocks
tests/
  test_executor.cpp   25 deterministic checks (strategy + OMS)
tools/                config builders, schema dump, fill simulator
deploy/setup_ec2.sh   provision a fresh EC2 box
```

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
   scheme. Do not build from memory.
3. **Key custody** decision (HSM/KMS/hot-wallet blast radius).
4. `LiveGateway` implementing `IExecGateway`: EIP-712 signer (pre-compute domain
   separator; pre-serialize templates), L2-authed REST POST/DELETE, the
   authenticated **user WS** for real fills, on-chain allowance setup.
5. **Fill-probability / adverse-selection model** from a long capture or tiny live
   posting — the only way to turn "gross ceiling" into a trustworthy net.

Until then: scanner is read-only; maker is shadow-only.

---

## Roadmap

1. **Capture-to-disk + offline replay** for `fill_sim.py` (hours/days) → a
   trustworthy net-of-adverse-selection number. *(Zero risk; do first.)*
2. Geography A/B (eu-west-1 vs others) using the `Feed deliv (ms)` line.
3. Validate the v2 fee formula against a non-zero `last_trade_price.fee_rate_bps`.
4. Live execution milestone (only after clearance) — see above.
5. Kernel bypass / CPU isolation — only worthwhile after geography, and likely
   never for a transatlantic maker.

---

*Scanner is read-only; the maker is shadow-only. No keys, no live orders, no money
at risk. Reward/PnL figures are gross, snapshot, order-of-magnitude estimates with
unproven net — not financial advice or a performance promise.*
