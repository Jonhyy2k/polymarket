# Dashboard guide & glossary

The dashboard (`dashboard/server.py` + `dashboard/index.html`, served on `:8080`) is a
single scrolling page of sections. This documents every section and **every column label**,
including the ones that are easy to misread. Hover any column header on the page itself —
they all carry a tooltip with the short version of what's below.

---

## Sections, top to bottom

| Section | What it shows |
|---|---|
| **KPIs** (top cards) | Cash, In-Orders $, Positions $, Total Value (click → portfolio chart), Earned today, Net Delta. |
| **MARKETS** | Live prices/book for the markets in `config.live.json` (the bot's configured set). Legacy arb-era columns — see glossary. |
| **WATCHLIST** | Markets *you* pinned with ★ from the screener. Live price/pool/exitability/affordability + ✦ if you hold a position. Stored in your browser. |
| **OPEN ORDERS** | Resting (unfilled) orders. Money committed but not yet a position. |
| **POSITIONS** | Tokens you actually **hold** from filled orders (≠ resting orders). |
| **REWARDS** | Per-market reward-program config + what you earned today. |
| **HISTORY & P&L** | Consolidated P&L summary + full account ledger (every trade / redeem / reward). |
| **SCREENER** | The whole reward-market universe, ranked by EXIT score. Search, star (★), one-click quote. |
| **PORTFOLIO RISK** | Binary-outcome stress models (what a goal/elimination/win does to you). |
| **SYSTEM** | Feed latency, reconnects, telemetry row count. |
| **CONTROLS** | Token-gated order placement / cancel / MM start-stop. |
| **Charts modal** | Rewards today (live) · Rewards all-time · Portfolio value. |

---

## Glossary — every column, plainly

### MARKETS table
- **YES / NO** — the outcome **mid-price** (midpoint of bid & ask). Untradeable reference; you buy at the ask, sell at the bid.
- **Δ SESS** — *"delta session."* How much the YES mid has moved **since this dashboard session started** (first price it saw). `+` = YES drifted up, `−` = down. It's a drift gauge, nothing more — it resets every time the dashboard restarts.
- **HI / LO** — session high / low of the YES mid.
- **BID / ASK** — best bid (where you could **sell** YES) / best ask (where you could **buy** YES).
- **SPR** — **spread** = ask − bid, in cents. Tighter = more liquid = easier to get in and out.
- **QMIN** — a reward-scoring constant used by the old arb engine to *estimate* your reward share. **Legacy** — the real reward economics now live in the SCREENER and REWARDS sections.
- **POOL/D** — total daily reward pool for the market (USDC/day).
- **EST·R** — a rough estimate of your daily reward. **Legacy**, treat as a hint only.
- **POS** — your net position (shares) in that market.
- **ST** — **status** dot: **● green** = both YES and NO sides are reward-eligible · **○ amber** = not eligible · **! red** = position flagged at risk. Legacy arb-engine indicator.

### SCREENER / WATCHLIST tables
- **EXIT** — **exitability rating**: how easily you can get *out* of a position. Score = reward-rate × book-tightness × touch-depth, bucketed **GOOD / OK / POOR**. This is the master filter: a fat reward rate in a wide, thin book is a **trap** (the Micron lesson — the mid said +42%, the real bid was 30¢ lower). Rank by *tradeable* reward, not raw reward.
- **$/DAY** — daily reward pool.
- **SPR¢** — live order-book spread in cents. Tighter = easier to exit.
- **DEPTH** — shares resting at the best bid/ask (touch depth). Deeper = easier to fill and exit.
- **BAND¢** — the **reward band** = the maximum distance from mid-price (in cents) your quote can sit and **still earn rewards**. Quote *outside* the band and you earn nothing, no matter how big the order. (Polymarket field: `rewards.max_spread`.)
- **MIN** — minimum order **size** (shares) required to qualify for rewards (`rewards.min_size`). Quote below this → no rewards, even two-sided. This is the floor that made rials pay us pennies (it's 50; we quoted 20).
- **2-SIDE $** — capital needed to rest `MIN` shares on **both** legs at once ≈ `min_size × (yes + no price)`. This is roughly **what you must commit to earn rewards in that market**. Shown **green ✓** when it fits your current cash. *This is the single most important column for a small account* — most fat pools sit behind a `min_size` you can't afford two-sided.
- **DTE** — days to market resolution.
- **★ / ☆** — add / remove the market from your Watchlist.

---

## Watchlist (★)
Click **☆** on any screener row (including search results) to pin a market to the WATCHLIST
section, where you can watch its live price, pool, exitability and affordability without
hunting for it. **✦** marks a market where you currently hold a position. The list is stored
in your browser (localStorage); the live data is resolved server-side via `/watchrows`, so
markets outside the top-60 screener still resolve.

## History & P&L
- **Total Value** = cash + positions (mark). **Rewards (all-time)** = cumulative LP rewards — the *reliable* number. **Unrealized P&L** = open positions at **mark** — honest caveat: thin books overstate this (you can only realize what the *bid* will pay).
- The ledger below lists every **TRADE / REDEEM / REWARD / CONVERSION** event from the on-chain
  activity feed (data-api `/activity`), newest first — your full paper trail.

## Reading P&L honestly (the one rule)
The dashboard marks positions at the **midpoint / last price**. That is *not* what you can
sell for. Always sanity-check against the **bid** and its **depth** before believing a green
number. A "+50%" on a 7-share bid is a mirage. EXIT / SPR¢ / DEPTH exist precisely to catch this.
