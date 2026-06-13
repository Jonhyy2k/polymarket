#include "rewards.hpp"

namespace {

// Snap a raw price (thousandths) to the tick grid. dir<0 floors (for bids),
// dir>0 ceils (for asks) so a quote never lands off-grid.
inline Price snap_to_tick(double price_thou, Price tick, int dir) noexcept {
    if (tick == 0) return 0;
    const double t = static_cast<double>(tick);
    const double n = price_thou / t;
    const double snapped = (dir < 0) ? std::floor(n) : std::ceil(n);
    long v = static_cast<long>(snapped) * tick;
    if (v < 0) v = 0;
    if (v > PRICE_ONE) v = PRICE_ONE;
    return static_cast<Price>(v);
}

}  // namespace

DesiredQuotes RewardQuoter::quote(const Orderbook& book, const RewardConfig& cfg,
                                  const RewardQuoteParams& params) noexcept {
    DesiredQuotes out{};

    // Need an active reward emission, a known tick, a two-sided book, and a size
    // that meets the qualifying minimum. Anything missing => no quote (we never
    // post off-grid or sub-min, which would simply not score).
    if (!cfg.active || cfg.max_spread_thou == 0 || cfg.min_size == 0) return out;
    if (book.tick_thou == 0) return out;
    if (params.quote_size < cfg.min_size) return out;

    const uint32_t mid2 = midpoint2_thou(book);
    if (mid2 == 0) return out;
    const uint32_t mid = mid2 / 2;  // integer midpoint for range tests

    const Price tick = book.tick_thou;
    // Tightest meaningful offset is one tick; honor a larger requested offset.
    const Price offset = std::max<Price>(tick, params.target_offset_thou);

    // Candidate prices: offset inside from mid on each side, snapped to grid.
    // Bid floors (stays at/below target), ask ceils (stays at/above target).
    Price bid_px = snap_to_tick(static_cast<double>(mid) - static_cast<double>(offset), tick, -1);
    Price ask_px = snap_to_tick(static_cast<double>(mid) + static_cast<double>(offset), tick, +1);

    // Do not cross the existing book (must remain a passive maker), and do not
    // cross each other.
    if (bid_px >= book.best_ask) bid_px = static_cast<Price>(book.best_ask - tick);
    if (ask_px <= book.best_bid) ask_px = static_cast<Price>(book.best_bid + tick);
    if (bid_px == 0 || ask_px == 0 || ask_px <= bid_px) return out;

    // Reward eligibility: each side must be within max_spread of mid (in 2x space
    // to keep the half-thousandth). Distance s2 = |2*price - mid2|.
    const auto within = [&](Price px) {
        const double s2 = std::abs(static_cast<double>(px) * 2.0 - static_cast<double>(mid2));
        return s2 < static_cast<double>(cfg.max_spread_thou) * 2.0;
    };

    if (within(bid_px)) {
        out.bid = {bid_px, params.quote_size, true};
    }
    if (within(ask_px)) {
        out.ask = {ask_px, params.quote_size, true};
    }

    // At mid extremes the program requires two-sided liquidity to score at all;
    // if either side fell outside max_spread there, the quote earns nothing.
    const bool central = mid >= 100 && mid <= 900;
    if (!central && !(out.bid.valid && out.ask.valid)) {
        out.bid = {}; out.ask = {};
        return out;
    }

    const double q_one = out.bid.valid
        ? spread_score(mid2, out.bid.price, cfg.max_spread_thou, out.bid.size) : 0.0;
    const double q_two = out.ask.valid
        ? spread_score(mid2, out.ask.price, cfg.max_spread_thou, out.ask.size) : 0.0;
    out.est_qmin = combine_qmin(q_one, q_two, mid);
    out.eligible = out.est_qmin > 0.0;
    return out;
}
