#pragma once

// Liquidity-rewards quoting strategy (Polymarket CLOB v2).
//
// Implements the verified reward scoring math (docs.polymarket.com/market-makers/
// liquidity-rewards, captured 2026-06-13):
//   - qualifying order: within max_spread of midpoint AND size >= min_size
//   - per-order spread score S(v,s) = ((v-s)/v)^2 * size   (v,s in same unit)
//   - per side Q_one / Q_two; combine:
//       mid in [0.10,0.90]: Qmin = max(min(Q1,Q2), max(Q1/c, Q2/c)), c = 3.0
//       mid at extremes:    Qmin = min(Q1,Q2)   (must be two-sided)
//   - reward share = Q_epoch / sum_all(Q_epoch) * daily pool, paid daily in USDC.
//
// The strategy posts a balanced two-sided quote as tight as the tick allows
// (tighter = higher score) while staying inside max_spread and not crossing.
// Reward is earned by STANDING in the book, not by filling — the right fit for a
// latency-disadvantaged operator. This module is pure (no I/O, no order state);
// the OMS turns DesiredQuotes into shadow/live order actions.

#include "types.hpp"

#include <cmath>
#include <algorithm>
#include <cstdint>

// Per-market reward configuration, as read from the CLOB API
// (GET /markets/{condition_id} -> rewards{min_size, max_spread, rates}).
struct RewardConfig {
    bool   active        = false;   // rates != null (market currently emits rewards)
    Price  max_spread_thou = 0;     // max_spread cents -> thousandths (4.5c => 45); 0 = unknown
    Size   min_size      = 0;       // min qualifying order size (shares)
    double daily_rate_usd = 0.0;    // sum of rewards_daily_rate across rate entries
};

struct RewardQuote {
    Price price = 0;
    Size  size  = 0;
    bool  valid = false;
};

struct DesiredQuotes {
    RewardQuote bid;   // BUY order resting below mid
    RewardQuote ask;   // SELL order resting above mid
    double      est_qmin = 0.0;  // estimated per-sample Qmin if both rest (telemetry)
    bool        eligible = false; // book + config allow a reward-qualifying quote
};

// Knobs for how aggressively to quote (the risk/reward dial).
struct RewardQuoteParams {
    Size  quote_size      = 0;   // size to post per side (>= min_size to qualify)
    Price target_offset_thou = 0; // desired distance from mid per side (0 => tightest = 1 tick)
};

// (best_bid+best_ask)/2 in thousandths*2 space to keep the half-cent exact; we
// return midpoint scaled by 2 (mid2) so callers avoid fractional rounding loss.
// Returns 0 if the book is not two-sided.
inline uint32_t midpoint2_thou(const Orderbook& book) noexcept {
    if (book.best_bid == 0 || book.best_ask == 0 || book.best_ask <= book.best_bid) return 0;
    return static_cast<uint32_t>(book.best_bid) + static_cast<uint32_t>(book.best_ask);
}

// Per-order quadratic spread score. v,s in thousandths; s measured from mid.
// Returns 0 when the order is outside max_spread (s >= v) or v is unknown.
inline double spread_score(uint32_t mid2_thou, Price order_price_thou,
                           Price max_spread_thou, Size size) noexcept {
    if (max_spread_thou == 0 || size == 0) return 0.0;
    // s = |order_price - mid| ; work in 2x space to keep the half-thousandth.
    const double s2 = std::abs(static_cast<double>(order_price_thou) * 2.0 -
                               static_cast<double>(mid2_thou));
    const double v2 = static_cast<double>(max_spread_thou) * 2.0;
    if (s2 >= v2) return 0.0;
    const double ratio = (v2 - s2) / v2;
    return ratio * ratio * static_cast<double>(size);
}

// Combine the two sides per the docs (c = 3.0). mid_price_thou is the integer
// midpoint (mid2/2) used only for the [0.10,0.90] range test.
inline double combine_qmin(double q_one, double q_two, uint32_t mid_price_thou) noexcept {
    constexpr double c = 3.0;
    const bool central = mid_price_thou >= 100 && mid_price_thou <= 900; // [0.10, 0.90]
    const double both = std::min(q_one, q_two);
    if (!central) return both;  // extremes: must be two-sided
    return std::max(both, std::max(q_one / c, q_two / c));
}

class RewardQuoter {
public:
    // Compute the desired two-sided quote for one token's book under its reward
    // config. Pure: no order state, no inventory yet (inventory skew comes later).
    static DesiredQuotes quote(const Orderbook& book, const RewardConfig& cfg,
                               const RewardQuoteParams& params) noexcept;
};
