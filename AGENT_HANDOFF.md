# Agent Handoff — Polymarket CLOB LP-rewards maker

> **For a Claude agent picking this up (e.g. on the Ireland EC2).** Read this top to
> bottom, then `README.md` + `git log` for detail. Written 2026-06-25.
> Latest `main` commit at handoff: **`b8ab604`** (PR #1 merged).

---

## 0. TL;DR — where we are

A low-latency **C++17** system for **Polymarket CLOB v2** that does two things, both
**non-live (NO keys, NO order signing, NO network orders, NO money)**:
1. **Arbitrage scanner** (read-only). Honest verdict: **taker edge ≈ 0** (efficient markets). Not the money-maker.
2. **Liquidity-rewards maker** (the actual strategy) — quotes two-sided near mid to earn Polymarket LP rewards, run in **shadow** or **mocklive** mode.

**Strategy direction:** pivoting from a "side provider" (fills rare, net ≈ gross) to a
**true LP** (expects fills) — which makes **adverse selection the entire P&L question,
and it is UNMEASURED.** That measurement is the #1 open task.

**Deployment:** AWS **c7i.4xlarge in eu-west-1 (Ireland)**, kernel-tuned and verified.
Host is now **optimal**: e2e **p50 ~3.2 µs, p99 ~4.7 µs, involuntary context-switches 0/s,
no memory leak**. Latency is now entirely the **~few-ms network to the London engine**.

**Live trading is GATED** on three things, none of which are code: (1) compliance/jurisdiction,
(2) on-chain EIP-712 verification, (3) the adverse-selection measurement.

---

## 1. The user + how to work with them

- Technically strong; **wants honest, critical pushback — not agreement.** Say "do this /
  skip this / that's wrong" with reasons. Half the value this session was rejecting bad
  ideas (e.g. host µs-optimizations that are noise vs the network; a non-compliant
  "London-cancel" instance split).
- Verify claims against the actual code before agreeing; several of their premises were
  already-done or misstated, and that's fine — correct them plainly.
- Lead with the framing that reorders priorities, then give a recommendation, not a survey.
- **Honesty rule:** distinguish *measured* from *assumed*. When you cite a latency number,
  say whether it's a 200k-iteration micro-bench (solid) or a 24-sample tail (noise). The
  user caught me overstating a small-sample p99 once — don't repeat that.

---

## 2. What this is (architecture)

Lock-free, pinned-thread pipeline; SPSC rings; allocation-free hot path.

```
 market WS (/ws/market, TLS)              CLOB/gamma REST (startup + periodic, read-only)
        │
   [receiver]  ── stamps recv steady+wall clock           [reward-refresh thread]
        │ SPSC (MessageSlot)                                    │ SPSC (RewardConfigUpdate)
        ▼                                                       ▼ (parser applies)
   [parser/main]  simdjson in-place → book → arb → executor+ACR
        │                                    │ SPSC (OrderCommand)   │ SPSC (metrics/opp/replay/qtel)
        ▼                                    ▼                       ▼
   (dead-man's-switch on feed-stale)   [cancel-sender]          [logger]
```

Key files (`src/`): `main.cpp` (wiring), `parser.hpp` (simdjson), `orderbook.cpp` (dense
ladder + occupancy bitmap), `arbitrage.cpp`, `rewards.{hpp,cpp}` (reward quoter),
`oms.{hpp,cpp}` (OMS + risk gate + ExecMode + ShadowGateway/MockLiveGateway seam),
`acr.hpp` (anti-cancel-race), `throttle.hpp`, `eip712.hpp` (Keccak/EIP-712, OpenSSL),
`live_order.hpp` (v2 order mapping), `mock_live_gateway.{hpp,cpp}`, `types.hpp`, `pipeline.hpp`.

**Execution modes** (`exec_mode`, OMS/ACR/risk are mode-agnostic):
- `shadow` — logs intended create/cancel only.
- `mocklive` — builds the **real v2 order + full EIP-712 digest** (no key, no sign, no send).
- `live` — **NOT built, gated**; falls back to shadow with a warning.

---

## 3. What was built this session (all on `main`)

- **Tier 1/2 (PR #1):** ExecMode 3-way split; `eip712.hpp` (real Keccak-256 digests);
  `MockLiveGateway` (validated: 12 digests on live reward markets); adaptive **quote throttle**
  (calm=slower, volatile=faster, ACR bypass); **pUSD allowance** sim; **quote-telemetry capture**
  (`quote_telemetry.csv` — the adverse-selection groundwork); TLS 1.3 + permessage-deflate A/B.
- **7 perf/quality audit fixes:** logger DRY; keccak cached domain-sep + reused thread-local
  EVP_MD_CTX (2.96→1.96 µs/digest); `ManagedOrder.token_id` → `string_view` (no ring-copy alloc);
  OMS MRU lookup cache; message-ring slot 1 MiB→256 KiB (reserved 128→32 MiB); dead-buffer
  removed; `alignas(64)` ring slots. ASAN/UBSAN clean.
- **Live-safety:** **dead-man's-switch** (feed-stale → cancel-all + halt new, resumes on data;
  `dead_mans_switch_seconds`); `Oms::set_kill_switch`; **startup reconciliation seam**
  (`IExecGateway::adopt_open_orders`, no-op in shadow/mock — the live plug-in point).
- **Reward-config refresh** (markets rotate `rates:null`; `reward_refresh_seconds`, single-writer ring).
- **`sender_park_after_idle_us`** (0 = pure busy-poll; >0 = park-when-idle for small boxes).
- **Tools:** `tools/verify_eip712.py` (on-chain EIP-712 reconcile), `tools/profile_run.py` +
  `analysis/profile_plots.py` (5-min /proc profiler + graphs), `tools/measure_latency.py`
  (network RTT), `deploy/tune_low_latency.sh` (host tuning: isolcpus/nosmt/governor/THP/IRQ/sysctl/limits).
- **Tests:** `test_executor` (35) + `live_test` (33), both green.

---

## 4. The honest conclusions (don't re-litigate)

- **Latency is network-bound.** In-process is ~3 µs; the network to London is ~few ms
  (~4 orders larger). Host µs-optimization is noise *while remote*. The decisive lever is
  **location** (done: Ireland) and eventually **KYB colocation** in eu-west-2 (London).
- **The cancel-race is not winnable from Ireland (~10 ms) vs colocated flow (~2 ms).** A
  rewards-LP must NOT depend on winning it; ACR's value is *smarts* (toxicity-aware cancels,
  optimal defensive spread), not speed.
- **Economics:** €800 Monte-Carlo (`analysis/returns_model.py`) → outcomes from **total ruin
  to ~$26k in 6 months, decided entirely by the unmeasured adverse-selection regime.**
  Capacity-bound, small-capital niche, gross %/day is a red flag not a forecast.
- **Returns from this work = $0.** Nothing is live. This work is deployment-readiness +
  measurement, not money.

---

## 5. The EC2 state (Ireland) — already done

- **Instance:** c7i.4xlarge (16 vCPU / 8 physical cores), eu-west-1. Ubuntu. SSH key `polyhft.pem`
  (chmod 600). Security group: SSH from My IP only; no inbound HTTP/HTTPS (bot is a client);
  outbound 443 default-allowed.
- **Kernel tuning applied** via `sudo deploy/tune_low_latency.sh --cores 2,3,4,5 --nosmt --apply-grub`
  then reboot, then re-run without `--apply-grub` for runtime bits. Verified:
  - `nosmt` → 8 cores online (`/sys/devices/system/cpu/online` = 0-7); `nproc --all`=16 just
    counts installed vCPUs (the 8 HT siblings are offline — not a problem).
  - `isolcpus`=2-5 (`/sys/devices/system/cpu/isolated` = 2-5). `nproc`=4 (shell sees only the
    4 non-isolated cores — this CONFIRMS isolation).
  - THP=`[never]`, chrony synced (Amazon Time Sync, Stratum 2), SCHED_FIFO works.
  - **cpufreq governor is N/A on virtualized EC2** (AWS controls frequency; expected, not a bug).
- **Config:** `config.live_ireland.json` (copy of `config.mocklive.json`) with pinning:
  `receiver_cpu:3, parser_cpu:2, logger_cpu:5, sender_cpu:4, realtime_priority:80,
  sender_priority:80, lock_memory:true, prefault_stack_kb:512, dead_mans_switch_seconds:5`.
  (`sender_park_after_idle_us` omitted = default 0 = pure busy-poll, correct for the isolated core.)
- **Profile result (5-min, the payoff):** involuntary context-switches **9.1/s (dev box) → 0/s**;
  e2e **p99 24 µs → 4.66 µs**; p50 ~3.2 µs; **RSS 178 MB locked & stable** (higher than the dev
  box's 51 MB because `lock_memory:true`/mlockall makes the reserved rings+stacks resident — intended).
  Residual p99.9 (~37 µs) / max (250 µs) is genuine **big-frame parse** (10–47 KB initial-dump /
  price-change bursts), NOT jitter — irrelevant vs the ms network.
- **Profiler note:** `profile_plots.py` text labels are hardcoded to cores 2/4/6 (dev-box pins);
  on the EC2 pins (2,3,4,5) the printed `cpu6` is an idle OS core, not the logger (cpu5). Graphs
  are correct. Optional cleanup: make it read pins from the config.

---

## 6. Restrictions & mandates (Polymarket + AWS)

- **Geoblock:** `/api/geoblock` checks IP vs 33 restricted countries (UK included). Order
  placement from a restricted region is rejected.
- **Engine in eu-west-2 (London)**, geo-restricted to operate from. **eu-west-1 (Ireland) is the
  designated unrestricted region** (we're there). **KYB colocation in eu-west-2** is the sanctioned
  sub-Cloudflare path (gated by compliance — NOT a workaround). Running infra in London to "cancel"
  is non-compliant (creates AND the account are at risk) — do not.
- **Public API is Cloudflare-fronted** (you → CF edge → London origin → back). From Ireland the
  edge is Dublin (`cf-ray …-DUB`).
- **v2 CLOB (since 2026-04-28):** new EIP-712 Order struct, domain version "2", pUSD collateral,
  taker-only fees + maker rebate. **Rewards live on sports/politics/event markets, NOT crypto**
  (`rates:null` ⇒ not emitting). Cloudflare rate limits → the quote throttle matters live.
- **v2 EIP-712 spec (verified-from-docs, UNVERIFIED on-chain):**
  - Order: `salt, maker, signer, tokenId, makerAmount, takerAmount, side(u8), signatureType(u8),
    timestamp, metadata(b32), builder(b32)`.
  - `ORDER_TYPEHASH = 0xbb86318a2138f5fa8ae32fbe8e659f8fcf13cc6ae4014a707893055433818589`.
  - Domain name "Polymarket CTF Exchange", version "2", chainId 137.
  - Exchange (standard) `0xE111180000d2663C0091e4f400237545B87B996B`; (neg-risk)
    `0xe2222d279d744050d28e00520010520000310F59`. signatureType 0=EOA,1=POLY_PROXY,2=POLY_GNOSIS_SAFE.
  - **OPEN:** the neg-risk domain `name` (default "Polymarket CTF Exchange"; a cheatsheet says
    "Polymarket Neg Risk CTF Exchange"). Resolve with `verify_eip712.py` + a real RPC (below).
  - Python keccak in `verify_eip712.py` reproduces the C++/OpenSSL values exactly (cross-checked).

---

## 7. Known limitations / breaks

- ⚠️ **Adverse selection UNMEASURED → net P&L unproven.** THE gap. The telemetry capture is built.
- ⚠️ **v2 EIP-712 UNVERIFIED on-chain** (neg-risk name). Don't trust any digest for a real signature
  until reconciled.
- ⚠️ v2 fee formula unvalidated (captured `fee_rate_bps` were 0).
- 🔒 **Compliance/jurisdiction (gate #1)** blocks live.
- ✅ Live-safety (DMS / kill / reconciliation seam) and reward-rotation are now handled.

---

## 8. THE PLAN — what to do next (ordered)

1. **Measure the real network RTT from the EC2** (replaces all estimates):
   - `git pull && python3 tools/measure_latency.py` — TCP-connect min = your→Dublin-edge RTT (~1 ms expected).
   - Run the bot and read `[PERF] TCP connect` + `[FEED] deliv (ms)` (chrony-trustworthy now). The
     CF→London leg is hidden behind Cloudflare; `[FEED]` (one-way) is the best read on the real leg.
2. **Reconcile the v2 EIP-712 on-chain** (resolves the neg-risk name → unblocks trustable digests):
   - `python3 tools/verify_eip712.py --rpc https://polygon-mainnet.g.alchemy.com/v2/<KEY>`
   - It has a stub-RPC guard (checks the CTF contract has code). Needs an authenticated Polygon RPC.
3. **Run `mocklive` for hours/days → capture adverse-selection telemetry** (the go/no-go number):
   - `./build/arb_detector --config config.live_ireland.json` → fills `logs/quote_telemetry.csv`.
   - Then fold into `tools/fill_sim.py` (replay reward markets, model fills + adverse selection at +30s)
     to produce a trustworthy **net = reward + inventory mark**. *This is the experiment that decides
     whether the true-LP shift makes money.* Probably the single most valuable thing to do.
4. **Improve ACR on the SMARTS axis** (only after #3 gives data): toxicity-aware cancels (book
   imbalance + trade direction), optimal defensive spread, calibrate thresholds to measured adverse selection.
5. **Compliance clearance (gate #1)** — out of scope for code; the user owns it.
6. **Build the `LiveGateway`** (ONLY after clearance + on-chain spec verified): ECDSA-sign over the
   MockLive digest (which already exists) + L2-authed REST POST/DELETE + on-chain allowances +
   the authenticated user WS for real fills + implement `adopt_open_orders` (startup reconciliation).

---

## 9. Key commands

```bash
# build + test
cd ~/polymarket && mkdir -p build && cd build && cmake .. && cmake --build . -j$(nproc)
./test_executor && ./live_test          # 35 + 33 pass
# run (mocklive, no keys/orders)
./build/arb_detector --config config.live_ireland.json
# profile the host (5 min)
python3 tools/profile_run.py --config config.live_ireland.json --duration 300 && python3 analysis/profile_plots.py
# measure network RTT
python3 tools/measure_latency.py
# verify EIP-712 on-chain (needs a real Polygon RPC)
python3 tools/verify_eip712.py --rpc <url>
# re-apply runtime tuning after a reboot (GRUB bits persist; governor/IRQ/sysctl don't)
sudo deploy/tune_low_latency.sh --cores 2,3,4,5
```

## 10. Hard guardrails (do not cross without the user explicitly clearing the gate)

- **NEVER** add live order signing/sending, keys, or anything touching money without (a) compliance
  cleared, (b) EIP-712 verified on-chain, (c) the user's explicit go-ahead. Everything stays
  shadow/mocklive until then.
- **NEVER** trust an EIP-712 digest for a real signature until `verify_eip712.py` passes on-chain.
- **NEVER** run trading infra in eu-west-2 (London) or circumvent the geoblock — it's non-compliant.
- Keep reward/PnL talk honest: gross, snapshot, unproven net — not forecasts.
