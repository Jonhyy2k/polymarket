#pragma once

// Anti-Cancel-Race (ACR) engine.
//
// Defensive layer for the liquidity-rewards maker: when the market moves so that
// a resting quote is about to be adversely filled (a taker is about to "pick off"
// our stale order), we want to cancel it FAST — before the fill. Detection runs
// INLINE on the parser thread (no thread hop on the critical path); the cancel
// SEND is offloaded to a dedicated sender thread (see ThreadedGateway) so the hot
// path never blocks on I/O.
//
// Three pieces:
//   1. at-risk detection  — a resting order has reached/crossed mid, or mid has
//      drifted >= N ticks against it since it was placed -> urgent cancel.
//   2. inventory skew      — shift quotes to mean-revert net position toward flat.
//   3. volatility widening — when |Δmid| EWMA is high, quote further from mid so a
//      jump is less likely to fill us.
//
// Pure/static and unit-tested; all policy lives here, no I/O.

#include "types.hpp"
#include "rewards.hpp"
#include "oms.hpp"

#include <cmath>
#include <algorithm>

struct AcrConfig {
    bool   enabled = true;
    int    stale_drift_ticks = 1;          // at-risk if mid drifted this many ticks against the order
    bool   cancel_on_cross = true;         // at-risk if a resting order reached/crossed mid
    double inv_skew_per_share_thou = 0.0;  // quote shift (thou) per share of net inventory, to flatten
    int    inv_skew_max_thou = 20;
    double vol_widen_k = 0.0;              // widen each side by k * acr_vol_thou
    int    vol_widen_max_thou = 20;
};

struct AcrRisk {
    bool bid_at_risk = false;
    bool ask_at_risk = false;
    bool any() const noexcept { return bid_at_risk || ask_at_risk; }
};

class AcrEngine {
public:
    // EWMA of |Δmid| (in thousandths). Call once per book update before assess().
    static void update_vol(Orderbook& book) noexcept {
        const uint32_t mid2 = midpoint2_thou(book);
        if (mid2 == 0) return;
        if (book.acr_last_mid2 != 0) {
            const float d = std::fabs(static_cast<float>(mid2) -
                                      static_cast<float>(book.acr_last_mid2)) * 0.5f;
            book.acr_vol_thou = 0.9f * book.acr_vol_thou + 0.1f * d;
        }
        book.acr_last_mid2 = mid2;
    }

    // Is a resting order about to be adversely filled?
    //   bid (we'd go long): danger when market falls -> bid reaches/crosses mid,
    //     or mid dropped >= drift since the bid was placed.
    //   ask (we'd go short): danger when market rises -> ask reaches/crosses mid,
    //     or mid rose >= drift since the ask was placed.
    static AcrRisk assess(const Orderbook& book, const LiveSide& live,
                          const AcrConfig& cfg) noexcept {
        AcrRisk r;
        if (!cfg.enabled) return r;
        const uint32_t mid2 = midpoint2_thou(book);
        if (mid2 == 0) return r;
        const Price tick = book.tick_thou ? book.tick_thou : 1;
        const uint32_t drift2 = static_cast<uint32_t>(cfg.stale_drift_ticks) * tick * 2u;

        if (live.bid.has) {
            const bool cross = cfg.cancel_on_cross && (2u * live.bid.price >= mid2);
            const bool drift = live.bid.ref_mid2 != 0 && mid2 + drift2 <= live.bid.ref_mid2;
            r.bid_at_risk = cross || drift;
        }
        if (live.ask.has) {
            const bool cross = cfg.cancel_on_cross && (2u * live.ask.price <= mid2);
            const bool drift = live.ask.ref_mid2 != 0 && mid2 >= live.ask.ref_mid2 + drift2;
            r.ask_at_risk = cross || drift;
        }
        return r;
    }

    // Apply inventory skew + volatility widening to the base desired quotes.
    //   long (net>0)  -> shift both quotes DOWN (ask cheaper = sell easier, bid
    //                    cheaper = buy harder) to mean-revert toward flat.
    //   high vol      -> push each side further from mid (bid down, ask up).
    static DesiredQuotes adjust(DesiredQuotes base, const Orderbook& book,
                                double net_position, const AcrConfig& cfg) noexcept {
        if (!cfg.enabled || !(base.bid.valid || base.ask.valid)) return base;
        const int tick = book.tick_thou ? book.tick_thou : 1;

        int skew = 0;
        if (cfg.inv_skew_per_share_thou != 0.0 && net_position != 0.0) {
            skew = static_cast<int>(std::llround(cfg.inv_skew_per_share_thou * net_position));
            skew = std::clamp(skew, -cfg.inv_skew_max_thou, cfg.inv_skew_max_thou);
        }
        int widen = 0;
        if (cfg.vol_widen_k != 0.0) {
            widen = static_cast<int>(std::llround(cfg.vol_widen_k *
                                                  static_cast<double>(book.acr_vol_thou)));
            widen = std::clamp(widen, 0, cfg.vol_widen_max_thou);
        }
        auto snap_down = [&](int p) { return (p / tick) * tick; };
        auto snap_up   = [&](int p) { return ((p + tick - 1) / tick) * tick; };

        if (base.bid.valid) {
            int np = snap_down(static_cast<int>(base.bid.price) - skew - widen);
            if (np <= 0) base.bid.valid = false; else base.bid.price = static_cast<Price>(np);
        }
        if (base.ask.valid) {
            int np = snap_up(static_cast<int>(base.ask.price) - skew + widen);
            if (np >= static_cast<int>(PRICE_ONE)) base.ask.valid = false;
            else base.ask.price = static_cast<Price>(np);
        }
        if (base.bid.valid && base.ask.valid && base.bid.price >= base.ask.price) {
            base.bid.valid = false;  // collapsed; drop the crossing side
        }
        return base;
    }
};
