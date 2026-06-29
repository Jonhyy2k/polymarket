# Market Selection & Spread Guide — how to choose what to quote, and where

A complete, ground-up guide to the two hardest judgment calls in Polymarket liquidity
market-making: **which market to quote** and **at what spread**. Everything here is
grounded in what we actually lived through (Micron trap, Hormuz pilot) — the numbers are
real, not theory.

---

## 0. What you're actually doing (the mental model)

You are **paid by Polymarket to post limit orders that other people can trade against.**
That's it. You are a liquidity provider (LP). Three — and only three — things make or lose
you money:

| # | Source | Variance | You want to… |
|---|---|---|---|
| 1 | **Liquidity rewards** | low, reliable | **maximize** — the designed edge |
| 2 | **Spread capture** (buy bid, sell ask, stay flat) | low | **maximize** — needs liquidity |
| 3 | **Directional P&L** from leftover inventory after a fill | **high**, adverse | **neutralize** — never chase |

The entire game is: **earn 1 + 2, and keep 3 as close to zero as possible.** Every mistake
we made was letting #3 (an accidental directional position) take over. Selection and spread
are the two levers that control how much #1 and #2 you get and how much #3 hurts.

---

## 1. The reward mechanics (what you're paid for)

Polymarket runs a **liquidity rewards program**. Each rewarded market has a daily **pool**
(`rate_per_day`, e.g. $300/day). Every epoch it's split among LPs by a score:

```
your_reward = (your_score / (everyone's_score + your_score)) × daily_pool
```

Your score rewards **two-sided** quoting **close to the midpoint**, **at size**, **inside a
spread band**. The market gives you these knobs/limits:

| Field | Meaning | Why it matters |
|---|---|---|
| `rate_per_day` | the daily $ pool | bigger = more to win, but draws competition |
| `max_spread` (the **band**, in ¢) | how far from mid your order may sit and still score | you MUST quote within it |
| `min_size` | minimum order size to count | sets your **capital floor** |
| `market_competitiveness` | proxy for how crowded the LP field is | higher = your slice is smaller |
| `tick` | price grid (e.g. 0.01) | every price must land on it |
| `end_date` / DTE | when it resolves | near-expiry = danger |

**Two hard rules that follow from this:**
- An order **below `min_size`** scores **nothing**. (A min_size-50 market needs ≥50-share orders.)
- An order **outside `max_spread`** of the mid scores **nothing**, even if it's resting.

---

## 2. PART A — Choosing the market

### 2.1 The capital floor (check this first)
A qualifying **two-sided** quote (BUY YES + BUY NO) costs about:

```
cost ≈ min_size × (1 − 2 × half_spread) ≈ min_size × 0.95
```

So **min_size 20 → ~$19**, **min_size 50 → ~$48**, **min_size 200 → ~$190**. If you can't
afford `min_size × ~0.95`, you **cannot earn rewards there** — full stop. (We learned this
the hard way: with $16 we couldn't fund a single qualifying quote; the floor is real.)

### 2.2 The master filter: EXITABILITY (the lesson of Micron)
**The #1 selection mistake is chasing a fat `rate_per_day` in a market you can't get out of.**
Micron paid ~$100/day but had a **36¢-wide book** with no depth — when we got filled we were
trapped: the midpoint "mark" said +42% but the only real bid was ~half that. **A reward you
can't exit isn't a reward; it's bait.**

So before anything else, look at the **order book**, not just the reward:

- **Book spread** = `best_ask − best_bid`. Tight (≤ ~2× the reward band) = good. Wide = trap.
- **Touch depth** = shares resting at the best bid / best ask. Deep relative to your size =
  you can exit. Thin = you'll move the price against yourself getting out.

> **Rule:** only quote where you can **exit at roughly the price you see**. The dashboard
> screener now computes this for you: `EXIT score = rate × spread_factor × depth_factor`,
> labelled GOOD / OK / POOR. Quote **GOOD**, occasionally **OK**, **never POOR**.

### 2.3 The full scoring — rank markets by *tradeable* reward
A good market maximizes this, roughly:

```
tradeable_score ≈ rate_per_day                       # the pool
                × (band / max(book_spread, band))    # tightness  (1.0 if book ≤ band)
                × min(touch_depth / min_size, 1)      # depth      (1.0 if deep enough)
                ÷ (1 + market_competitiveness)        # crowding   (your slice)
                × dte_safety                          # 0 near expiry, 1 when safe
```

In plain words, prefer markets that are: **well-paid, tight, deep, uncrowded, not near
resolution, and affordable.** You will rarely max all six — rank and take the best
*risk-adjusted* one, not the biggest headline rate.

### 2.4 Competition — what high vs low means *for your wallet*
`market_competitiveness` is the crowding proxy. It doesn't change your **risk**; it changes
your **share of the pool**:
- **Low (≤0.2):** few LPs → you keep most of the pool → high $/$ deployed.
- **Medium (~0.5–1.5):** shared → modest share. (Hormuz was 1.49.)
- **High (>1.5):** flooded → your $19 is a rounding error in the field → tiny share.

Low competition is usually found in **boring, smaller, longer-dated** markets that pros
ignore — which is exactly where a small account should fish.

### 2.5 DTE (days to expiry) — the safety rail
- **DTE < 1 day:** avoid. Price can gap on resolution news; a fill can lock a loss you can't
  exit before it settles to $0/$1.
- **DTE a few days–months:** ideal. Room to quote, exit, and never hold inventory *into* the
  event. (Hormuz had 62 days — deliberately chosen so the test had no resolution risk.)
- **Always flatten inventory ~24h before resolution**, regardless.

### 2.6 Price sanity (avoid degenerate books)
Prefer markets priced roughly **0.15–0.85**. Near 0/1 (e.g. YES at 0.02) the book is
one-sided and illiquid, the tick is a huge % move, and you can't quote both sides sensibly.

### 2.7 Red flags — skip if you see these
- Book spread ≫ reward band (illiquid trap)
- Touch depth < your size (can't exit)
- `min_size × 0.95` > your affordable capital
- DTE < 1 day
- Price < 0.12 or > 0.88
- `market_competitiveness` very high *and* the pool isn't large enough to matter
- `rate_per_day` shown as 0 / `max_spread` 0 (these markets **don't actually pay**)

### 2.8 Selection checklist (run top-to-bottom)
1. Can I afford it? `min_size × 0.95 ≤ my cash` → else skip.
2. EXIT label GOOD/OK? book spread tight, depth ≥ my size → else skip.
3. DTE comfortable (> a few days)? → else skip.
4. Price in 0.15–0.85? → else skip.
5. Competition acceptable for the pool size? → prefer low.
6. Among survivors, take the **highest tradeable_score**.

---

## 3. PART B — Choosing the spread (where to place the quotes)

### 3.1 The structure: cash-only two-sided
We post **BUY YES @ (mid_yes − h)** and **BUY NO @ (mid_no − h)**, both **post-only**, both
**GTD**. A BUY-NO at price `p` is economically a SELL-YES at `1−p`, so the pair quotes *both
sides* of the book and earns two-sided reward score — funded entirely by cash, no inventory
needed. `h` is the **half-spread**: how far below the mid each bid sits.

### 3.2 The central trade-off (this is the whole skill)
Where you set `h` inside the band is a three-way tug-of-war:

| Quote **tight** (small `h`, near mid) | Quote **wide** (large `h`, near band edge) |
|---|---|
| ✅ higher reward score (closer to mid) | ❌ lower reward score |
| ✅ more fills | ✅ fewer fills |
| ❌ **more adverse selection** (you get lifted right before moves) | ✅ less adverse selection |
| ❌ more inventory churn to manage | ✅ calmer |

**There is no free lunch:** the same closeness-to-mid that earns more reward also gets you
filled more, and fills are how #3 (the directional leak) hurts you. The right `h` depends on
the number we're measuring — adverse selection. Until we have it, **start conservative**
(`h ≈ 0.02`, well inside a 4.5¢ band): you still score, you fill less, you bleed less.

### 3.3 Hard constraints on `h`
- **`h ≤ max_spread`** (in price units) or you score **zero** reward. Band 4.5¢ = 0.045 → keep
  `h ≤ ~0.04`. With margin, 0.02–0.03 is the sweet spot.
- **`h` large enough to stay post-only** — your bid must land **at or below the best bid** so
  it never crosses. In a 1¢-wide book, `mid − 0.02` is already ~1.5¢ below the bid: safe.
- **Round to tick.** Every price must sit on the grid (0.01 here). The engine rounds for you,
  but know that 0.575 is **not** a valid price — only 0.57 / 0.58.

### 3.4 Worked spread example (Hormuz)
- YES mid 0.385, NO mid 0.615, band 4.5¢, tick 0.01, `h = 0.02`.
- Quotes: **BUY YES @ 0.36** (0.385 − 0.02 → tick), **BUY NO @ 0.59** (0.615 − 0.02 → tick).
- Distance from mid = ~2¢ ≤ 4.5¢ band → **scores reward**. ✅
- Both rest **below** the touch (YES bid 0.38, NO bid 0.61) → **post-only safe**, fill only if
  price comes to us → **conservative.** ✅
- Capital: 20×0.36 + 20×0.59 ≈ **$19** ✅

### 3.5 What to do AFTER a fill (inventory handling)
Fills are inevitable; mishandling them is the whole risk. The discipline, in order:
1. **Skew** — the instant you're long a side, **stop quoting that side** (don't double down).
   *(The bot now does this automatically.)*
2. **Scratch** — post a SELL to exit the inventory at `fill + a tick or two` (capture the
   spread you were owed), or at `fill` for a flat scratch.
3. **Stop** — if it runs against you past a set distance, **cut it** (small loss > hope).
4. **DTE-flatten** — never hold inventory into resolution; flatten ~24h before.
> **You cannot do this by hand while the MM runs** — its `cancel_all` erases your manual
> orders and it has no sell logic. Either the bot does it automatically (the next build) or
> you turn the MM **off** and manage manually. Never both.

### 3.5b The legged fill, in depth — patience, the reward cushion, reversion
A one-sided ("legged") fill is **not automatically a loss** — it's *half a round-trip waiting
for the other half.* The drawdown while you wait is **unrealized** (it only becomes real if
you sell into it, it trends, or it resolves against you), and **you keep earning LP rewards**
the whole time. So the patient play is usually:

> Hold the leg (skew) → keep quoting the other side → **let rewards accrue while you wait for
> the market to oscillate back** → if it reverts, the other leg fills and you complete the
> risk-free <$1 set (+ all the rewards banked); if it trends, the stop cuts it small.

**The rewards are the cushion that buys you the patience to let oscillation finish the
round-trip.** This works precisely when, and only when:

| Condition | Needed because |
|---|---|
| **market reverts (range-bound, not trending)** | the second leg fills only if price oscillates back |
| **reward rate ≥ adverse-selection bleed rate** | the cushion must outpace the drawdown while you wait (the measurable break-even) |
| **you're not forced out** (DTE far, no premature stop) | a reverting drawdown is harmless *if you can wait it out* |

Caveat: while legged you earn the **one-sided** (reduced) reward, not the full two-sided rate,
so the cushion is smaller until the hedge completes — and capital is locked while you wait.

**Numeric feel.** NO fills @ 0.59 (20 NO = $11.80); NO dips to 0.55 → −$0.80 *unrealized*, but
you keep resting BUY YES + earning rewards.
- *Reverts:* NO back to ~0.59, YES dips into your 0.36 bid → hedged set for ~0.95 → **+$0.05 +
  rewards**; the −$0.80 never happened.
- *Trends:* NO keeps falling, YES never fills → bleed > rewards → **stop-loss cuts it small.**

**Implication for selection:** favour **range-bound, liquid, low-drama** markets (more
reversion, smaller swings, both legs fill) over **event-driven / trending** ones (one-way
moves leg you and don't come back). The reward-vs-bleed inequality is the number to measure.

### 3.6 Order types (know which to use)
| Type | Behaviour | Use for |
|---|---|---|
| **GTC** | rests until filled/cancelled | a manual order you'll manage |
| **GTD** | rests until a timestamp, then auto-expires | **MM quotes** (dies with the bot — safety) |
| **FAK** | fill what crosses now, kill the rest | **exiting fast** (sweep the bid, leave nothing) |
| **FOK** | all-or-nothing immediately | rare |

---

## 4. PART C — The economics (the only number that matters)

Everything reduces to one inequality, per market:

```
        reward_$/day   >   adverse_selection_bleed_$/day      → profitable
        reward_$/day   <   adverse_selection_bleed_$/day      → you bleed while feeling busy
```

- **reward/day** you can estimate up front: `pool × your_share`, where share ≈
  `your_size / (field_size + your_size)` (use `market_competitiveness` as the field proxy).
- **adverse bleed/day** you can **only measure** — it's the average mark-out on your fills ×
  fill frequency. That's what `tools/fill_telemetry.py` exists to capture (mid at
  +1m/+5m/+15m after each fill; negative = adverse).

**You do not know if a strategy is profitable until you've measured the bleed in that market.**
This is why we pilot small and measure before scaling. (First Hormuz data point: mark-out
was *positive* — encouraging, but n=1.)

### 4.1 Capital tiers (what's realistic)
| Capital | Concurrent qualifying quotes | Verdict |
|---|---|---|
| ~$20 | 1 (min_size 20) | proof-of-concept; validate mechanics, not income |
| ~$80–200 | a few | starts to make sense; can diversify a little |
| ~$500–1k | 5–20 | the strategy in its element; diversified, edge compounds |
| $1k+ | many | watch capacity / competition compression on big pools |

---

## 5. PART D — Order & fill mechanics (the gotchas that bit us)

- **Bid vs ask vs mid.** You **buy at the ask**, **sell at the bid**. The **mid** (what the
  dashboard shows as "price") is **untradeable** — you can't sell at the mid. To **sell now**
  you must price **≤ the best bid**; anything above just rests. (This is why "sell at 0.58"
  didn't fill when the bid was 0.56.)
- **Maker vs taker.** Resting (post-only) = **maker** (earns rewards, pays no taker fee).
  Crossing the spread to fill now = **taker** (pays a small fee, ~1–2%). Exits are often taker.
- **Inventory reserves orders.** A resting SELL for your whole position **locks** that
  inventory — you can't place a second sell until you cancel the first. (This is why a second
  sell got rejected.)
- **`cancel_all` is indiscriminate.** The MM cancels *every* order on the account on each
  re-quote and on shutdown — including manual ones. MM + hand orders don't coexist.
- **The midpoint mark lies in illiquid books.** `currentValue` / `cashPnl` from the data API
  use the **mid**; in a wide book that's far above what you can actually realize. Trust the
  **bid** for "what can I sell for right now."

---

## 6. PART E — The two case studies (burned into the strategy)

**Micron (the trap).** Fat rate, but 36¢ book, no depth, min_size 50. Got legged into 20 NO
overnight (box was off, GTC order naked — since fixed with GTD). The mid said +42%; the real
exit was ~breakeven. Round-trip −$0.99; rewards +$1.29; **net ~flat.** Lesson: **illiquid =
trap; rank by exitability; never leave naked orders.**

**Hormuz (the pilot).** GOOD-exit market (1¢ book, deep), min 20, DTE 62, chosen by the new
exitability screener. Quoted $19 two-sided at `h=0.02`. Got filled on NO; **skew worked**
(stopped adding); mark-out came back **positive**; exited ~breakeven on the trade but **+$0.60
net with rewards.** Lesson: **a properly-selected market behaves; the rewards can cover a
small adverse fill — which is the entire thesis.**

---

## 7. One-page quick reference

**Pick a market when ALL of:**
- [ ] `min_size × 0.95 ≤ your cash`
- [ ] EXIT = GOOD/OK (tight book, depth ≥ your size)
- [ ] DTE > a few days
- [ ] price 0.15–0.85
- [ ] competition acceptable for the pool size
- [ ] highest `tradeable_score` among survivors

**Place quotes:**
- two-sided, post-only, GTD
- `h ≈ 0.02`, always `h ≤ max_spread`, on-tick, below the touch
- size ≥ `min_size`

**After a fill:** skew → scratch → stop → DTE-flatten. Bot does it, or MM off and you do — never both.

**The decider:** reward_$/day vs adverse_bleed_$/day. Measure before you scale.

*Bottom line: the edge is not speed and not the biggest headline reward. It's **selecting
markets you can exit** and **quoting wide enough to bleed less than you earn.** Everything
else is noise.*
