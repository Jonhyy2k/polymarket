#pragma once

// Adaptive quote throttle.
//
// Re-quoting on every book tick is fine in shadow, but live it shreds queue
// priority and burns rate limits. This gate suppresses pure RE-PRICING churn
// between moves while never blocking an action that must go out now:
//   - ACR at-risk  -> must cancel immediately (bypass)
//   - a side being added or removed (eligibility change) -> bypass
//   - otherwise    -> hold the resting quote until the adaptive interval elapses
//
// Adaptation (this is the corrected logic — calm lengthens, volatile shortens):
//   calm market   -> grow the interval toward max  (quote less, keep queue spot)
//   volatile market-> shrink the interval toward min (re-center faster)
//
// State lives on the Orderbook (thr_last_quote_ns / thr_interval_ms), so it is
// O(1) per token with no map. Pure policy; unit-tested.

#include "types.hpp"

#include <algorithm>
#include <cstdint>

struct ThrottleConfig {
    bool     enabled = false;
    uint32_t min_interval_ms = 5;
    uint32_t max_interval_ms = 100;
    float    vol_hot_thou = 2.0f;   // acr_vol_thou at/above this => volatile
};

class QuoteThrottle {
public:
    // Returns true if a reconcile is allowed for this book right now.
    // must_act bypasses the throttle (ACR / side add-remove / first quote).
    static bool allow(Orderbook& book, const ThrottleConfig& cfg,
                      uint64_t now_ns, bool must_act) noexcept {
        if (!cfg.enabled) return true;
        if (must_act) {
            book.thr_last_quote_ns = now_ns;
            book.thr_interval_ms = cfg.min_interval_ms;  // be responsive after a forced act
            return true;
        }
        // thr_interval_ms == 0 is the "never quoted yet" marker — robust against a
        // now_ns that happens to be 0 (unlike using the timestamp as the sentinel).
        const bool first = (book.thr_interval_ms == 0);
        if (!first) {
            const uint64_t elapsed_ms = (now_ns - book.thr_last_quote_ns) / 1000000ull;
            if (elapsed_ms < book.thr_interval_ms) {
                return false;  // throttled: hold the resting quote, preserve priority
            }
        }

        // Allowed now — adapt the cadence for next time. Calm lengthens (quote
        // less, keep queue spot); volatile shortens (re-center faster).
        const bool hot = book.acr_vol_thou >= cfg.vol_hot_thou;
        uint32_t iv = first ? cfg.min_interval_ms : book.thr_interval_ms;
        if (hot) iv = (iv > cfg.min_interval_ms) ? (iv / 2) : cfg.min_interval_ms;
        else     iv = iv * 2;
        book.thr_interval_ms = std::clamp(iv, cfg.min_interval_ms, cfg.max_interval_ms);
        book.thr_last_quote_ns = now_ns;
        return true;
    }
};
