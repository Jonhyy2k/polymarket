#include "arbitrage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

inline int bps_to_thousandths(int bps) noexcept {
    return (bps >= 0) ? (bps + 5) / 10 : (bps - 5) / 10;
}

inline int thousandths_to_bps(int thousandths) noexcept {
    return thousandths * 10;
}

inline int64_t theoretical_edge_score(int net_edge_thou, Size size_shares) noexcept {
    return static_cast<int64_t>(net_edge_thou) * static_cast<int64_t>(size_shares);
}

// Maker quote one tick inside the spread. Polymarket ticks are per-market
// (0.001 mid-range, 0.01 near 0.50, etc.), so we MUST step by the book's real
// tick or the quote lands off-grid and is unplaceable. If the tick is unknown
// (no book/gamma seed yet) we refuse to quote rather than guess.
inline Price inside_buy_price(const Orderbook& book) noexcept {
    if (book.best_bid == 0 || book.best_ask == 0 || book.tick_thou == 0) return 0;
    const uint32_t candidate = static_cast<uint32_t>(book.best_bid) + book.tick_thou;
    return (candidate < book.best_ask && candidate <= PRICE_ONE) ? static_cast<Price>(candidate) : 0;
}

inline Price inside_sell_price(const Orderbook& book) noexcept {
    if (book.best_ask == 0 || book.tick_thou == 0 || book.best_ask <= book.tick_thou) return 0;
    const Price candidate = static_cast<Price>(book.best_ask - book.tick_thou);
    return (candidate > book.best_bid) ? candidate : 0;
}

template <typename Output>
inline void record_edge_sample(Output& out, NanoTime t2_book_updated, std::string_view label,
                               ArbKind kind, uint16_t reference_value, uint8_t leg_count,
                               int raw_edge_thou, int net_edge_thou) {
    if (out.edge_sample_count >= out.edge_samples.size()) return;
    EdgeTelemetrySample& sample = out.edge_samples[out.edge_sample_count++];
    sample = {};
    sample.sample_time_ns = t2_book_updated;
    sample.label = label;
    sample.arb_kind = kind;
    sample.reference_value = reference_value;
    sample.leg_count = leg_count;
    sample.raw_edge_bps = static_cast<int16_t>(thousandths_to_bps(raw_edge_thou));
    sample.net_edge_bps = static_cast<int16_t>(thousandths_to_bps(net_edge_thou));
    sample.valid = true;
}

struct BestFocalOpportunity {
    const Contract* focal = nullptr;
    uint16_t reference_value = 0;
    int raw_edge_thou = std::numeric_limits<int>::min();
    int net_edge_thou = std::numeric_limits<int>::min();
    Size size_shares = 0;

    void consider(const Contract* candidate_focal, uint16_t candidate_reference_value,
                  int candidate_raw_edge_thou, int candidate_net_edge_thou,
                  Size candidate_size_shares) noexcept {
        if (candidate_focal == nullptr || candidate_size_shares == 0) return;

        if (focal == nullptr ||
            theoretical_edge_score(candidate_net_edge_thou, candidate_size_shares) >
                theoretical_edge_score(net_edge_thou, size_shares) ||
            (theoretical_edge_score(candidate_net_edge_thou, candidate_size_shares) ==
                 theoretical_edge_score(net_edge_thou, size_shares) &&
             candidate_net_edge_thou > net_edge_thou)) {
            focal = candidate_focal;
            reference_value = candidate_reference_value;
            raw_edge_thou = candidate_raw_edge_thou;
            net_edge_thou = candidate_net_edge_thou;
            size_shares = candidate_size_shares;
        }
    }
};

}  // namespace

ArbitrageDetector::ArbitrageDetector(const Config& config) : config_(config) {}

int ArbitrageDetector::per_share_fee_thou(const Contract& contract, Price price) const {
    if (price == 0) return 0;

    // FAIL CLOSED: only assume a market is genuinely fee-free when we positively
    // parsed its fee schedule and it reports fees disabled. A parse miss or an
    // un-fetched market must NOT collapse to zero fees (that manufactures edge,
    // and with min_edge_threshold_bps=0 there is no buffer). In any uncertain
    // case we charge the configured fallback.
    if (contract.fee_metadata_valid) {
        if (!contract.fee_schedule_enabled || contract.fee_rate <= 0.0) {
            return 0;  // positively known: this market does not charge fees
        }
        // NOTE: V2 fee formula is NOT yet validated against a real charged fee
        // (captured last_trade_price samples showed fee_rate_bps=0, likely maker
        // fills under taker-only fees). Keep the prior form but treat its output
        // as an upper-bound estimate; revisit once a non-zero fee_rate_bps lands.
        const double p = static_cast<double>(price) / static_cast<double>(PRICE_ONE);
        const double fee_usdc = p * contract.fee_rate *
                                std::pow(p * (1.0 - p), contract.fee_exponent);
        return static_cast<int>(std::llround(fee_usdc * static_cast<double>(PRICE_ONE)));
    }

    const int fallback_fee_bps =
        contract.taker_fee_bps_override >= 0 ? contract.taker_fee_bps_override : config_.taker_fee_bps;
    return bps_to_thousandths(fallback_fee_bps);
}

ArbCheckOutput ArbitrageDetector::check(const Contract& contract, NanoTime t2_book_updated) {
    ArbCheckOutput out{};

    const auto& yes = contract.book_yes;
    const auto& no = contract.book_no;
    const int min_edge_thou = bps_to_thousandths(config_.min_edge_threshold_bps);

    const auto emit_opp = [&](std::string_view label, ArbKind kind, uint16_t cost_or_proceeds,
                              int raw_edge_thou, int net_edge_thou, Size size) {
        if (size == 0 || out.opportunity_count >= out.opportunities.size()) return;

        ArbOpportunity& opp = out.opportunities[out.opportunity_count++];
        opp = {};
        opp.t2_book_updated_ns = t2_book_updated;
        opp.t3_arb_checked_ns = now_ns();
        opp.contract_name = label;
        opp.arb_kind = kind;
        opp.ask_yes = yes.best_ask;
        opp.ask_no = no.best_ask;
        opp.bid_yes = yes.best_bid;
        opp.bid_no = no.best_bid;
        opp.cost_or_proceeds = cost_or_proceeds;
        opp.raw_edge_bps = static_cast<int16_t>(thousandths_to_bps(raw_edge_thou));
        opp.edge_bps = static_cast<int16_t>(thousandths_to_bps(net_edge_thou));
        opp.size_shares = size;
        opp.leg_count = 2;
        opp.theoretical_pnl = static_cast<double>(net_edge_thou) / 1000.0 * size;

        ++total_opps_;
        switch (kind) {
            case ArbKind::BUY_BOTH:
                ++buy_both_;
                break;
            case ArbKind::SELL_BOTH:
                ++sell_both_;
                break;
            case ArbKind::BUY_GROUP_YES:
                ++buy_group_;
                break;
            case ArbKind::SELL_GROUP_YES:
                ++sell_group_;
                break;
            case ArbKind::BUY_NO_SELL_OTHERS:
                ++buy_no_sell_others_;
                break;
            case ArbKind::SELL_NO_BUY_OTHERS:
                ++sell_no_buy_others_;
                break;
            case ArbKind::MAKER_BUY_BOTH:
                ++maker_buy_both_;
                break;
            case ArbKind::MAKER_SELL_BOTH:
                ++maker_sell_both_;
                break;
            case ArbKind::MAKER_BUY_GROUP_YES:
                ++maker_buy_group_;
                break;
            case ArbKind::MAKER_SELL_GROUP_YES:
                ++maker_sell_group_;
                break;
            case ArbKind::MAKER_BUY_NO_SELL_OTHERS:
                ++maker_buy_no_sell_others_;
                break;
            case ArbKind::MAKER_SELL_NO_BUY_OTHERS:
                ++maker_sell_no_buy_others_;
                break;
        }
    };

    if (yes.best_ask > 0 && no.best_ask > 0 &&
        yes.best_ask_size > 0 && no.best_ask_size > 0) {
        ++total_checks_;
        ++out.checks_performed;

        const uint16_t cost = yes.best_ask + no.best_ask;
        const int raw_edge_thou = static_cast<int>(PRICE_ONE) - static_cast<int>(cost);
        const int fee_cost_thou =
            per_share_fee_thou(contract, yes.best_ask) + per_share_fee_thou(contract, no.best_ask);
        const int net_edge_thou = raw_edge_thou - fee_cost_thou;

        record_edge_sample(out, t2_book_updated, contract.asset_name, ArbKind::BUY_BOTH,
                           cost, 2, raw_edge_thou, net_edge_thou);

        if (net_edge_thou > min_edge_thou) {
            emit_opp(contract.asset_name, ArbKind::BUY_BOTH, cost, raw_edge_thou,
                     net_edge_thou, std::min(yes.best_ask_size, no.best_ask_size));
        }
    }

    if (yes.best_bid > 0 && no.best_bid > 0 &&
        yes.best_bid_size > 0 && no.best_bid_size > 0) {
        ++total_checks_;
        ++out.checks_performed;

        const uint16_t proceeds = yes.best_bid + no.best_bid;
        const int raw_edge_thou = static_cast<int>(proceeds) - static_cast<int>(PRICE_ONE);
        const int fee_cost_thou =
            per_share_fee_thou(contract, yes.best_bid) + per_share_fee_thou(contract, no.best_bid);
        const int net_edge_thou = raw_edge_thou - fee_cost_thou;

        record_edge_sample(out, t2_book_updated, contract.asset_name, ArbKind::SELL_BOTH,
                           proceeds, 2, raw_edge_thou, net_edge_thou);

        if (net_edge_thou > min_edge_thou) {
            emit_opp(contract.asset_name, ArbKind::SELL_BOTH, proceeds, raw_edge_thou,
                     net_edge_thou, std::min(yes.best_bid_size, no.best_bid_size));
        }
    }

    if (config_.maker_arb_enabled) {
        {
            int best_raw_edge_thou = std::numeric_limits<int>::min();
            int best_net_edge_thou = std::numeric_limits<int>::min();
            uint16_t best_cost = 0;
            Size best_size = 0;

            const Price yes_inside = inside_buy_price(yes);
            if (yes_inside > 0 && no.best_ask > 0 && no.best_ask_size > 0) {
                const uint16_t cost = static_cast<uint16_t>(yes_inside + no.best_ask);
                const int raw_edge_thou = static_cast<int>(PRICE_ONE) - static_cast<int>(cost);
                const int fee_cost_thou =
                    per_share_fee_thou(contract, yes_inside) + per_share_fee_thou(contract, no.best_ask);
                const int net_edge_thou = raw_edge_thou - fee_cost_thou;
                if (net_edge_thou > best_net_edge_thou) {
                    best_raw_edge_thou = raw_edge_thou;
                    best_net_edge_thou = net_edge_thou;
                    best_cost = cost;
                    best_size = no.best_ask_size;
                }
            }

            const Price no_inside = inside_buy_price(no);
            if (no_inside > 0 && yes.best_ask > 0 && yes.best_ask_size > 0) {
                const uint16_t cost = static_cast<uint16_t>(yes.best_ask + no_inside);
                const int raw_edge_thou = static_cast<int>(PRICE_ONE) - static_cast<int>(cost);
                const int fee_cost_thou =
                    per_share_fee_thou(contract, yes.best_ask) + per_share_fee_thou(contract, no_inside);
                const int net_edge_thou = raw_edge_thou - fee_cost_thou;
                if (net_edge_thou > best_net_edge_thou) {
                    best_raw_edge_thou = raw_edge_thou;
                    best_net_edge_thou = net_edge_thou;
                    best_cost = cost;
                    best_size = yes.best_ask_size;
                }
            }

            if (best_net_edge_thou != std::numeric_limits<int>::min()) {
                ++total_checks_;
                ++out.checks_performed;
                record_edge_sample(out, t2_book_updated, contract.asset_name, ArbKind::MAKER_BUY_BOTH,
                                   best_cost, 2, best_raw_edge_thou, best_net_edge_thou);
                if (best_net_edge_thou > min_edge_thou) {
                    emit_opp(contract.asset_name, ArbKind::MAKER_BUY_BOTH, best_cost,
                             best_raw_edge_thou, best_net_edge_thou, best_size);
                }
            }
        }

        {
            int best_raw_edge_thou = std::numeric_limits<int>::min();
            int best_net_edge_thou = std::numeric_limits<int>::min();
            uint16_t best_proceeds = 0;
            Size best_size = 0;

            const Price yes_inside = inside_sell_price(yes);
            if (yes_inside > 0 && no.best_bid > 0 && no.best_bid_size > 0) {
                const uint16_t proceeds = static_cast<uint16_t>(yes_inside + no.best_bid);
                const int raw_edge_thou = static_cast<int>(proceeds) - static_cast<int>(PRICE_ONE);
                const int fee_cost_thou =
                    per_share_fee_thou(contract, yes_inside) + per_share_fee_thou(contract, no.best_bid);
                const int net_edge_thou = raw_edge_thou - fee_cost_thou;
                if (net_edge_thou > best_net_edge_thou) {
                    best_raw_edge_thou = raw_edge_thou;
                    best_net_edge_thou = net_edge_thou;
                    best_proceeds = proceeds;
                    best_size = no.best_bid_size;
                }
            }

            const Price no_inside = inside_sell_price(no);
            if (no_inside > 0 && yes.best_bid > 0 && yes.best_bid_size > 0) {
                const uint16_t proceeds = static_cast<uint16_t>(yes.best_bid + no_inside);
                const int raw_edge_thou = static_cast<int>(proceeds) - static_cast<int>(PRICE_ONE);
                const int fee_cost_thou =
                    per_share_fee_thou(contract, yes.best_bid) + per_share_fee_thou(contract, no_inside);
                const int net_edge_thou = raw_edge_thou - fee_cost_thou;
                if (net_edge_thou > best_net_edge_thou) {
                    best_raw_edge_thou = raw_edge_thou;
                    best_net_edge_thou = net_edge_thou;
                    best_proceeds = proceeds;
                    best_size = yes.best_bid_size;
                }
            }

            if (best_net_edge_thou != std::numeric_limits<int>::min()) {
                ++total_checks_;
                ++out.checks_performed;
                record_edge_sample(out, t2_book_updated, contract.asset_name, ArbKind::MAKER_SELL_BOTH,
                                   best_proceeds, 2, best_raw_edge_thou, best_net_edge_thou);
                if (best_net_edge_thou > min_edge_thou) {
                    emit_opp(contract.asset_name, ArbKind::MAKER_SELL_BOTH, best_proceeds,
                             best_raw_edge_thou, best_net_edge_thou, best_size);
                }
            }
        }
    }

    return out;
}

ArbCheckOutput ArbitrageDetector::check_group(const ContractGroup& group, NanoTime t2_book_updated) {
    ArbCheckOutput out{};
    if (!group.exhaustive || group.contracts.size() < 2) {
        return out;
    }

    const int min_edge_thou = bps_to_thousandths(config_.min_edge_threshold_bps);
    const uint8_t leg_count = static_cast<uint8_t>(group.contracts.size());

    const auto emit_group_opp = [&](std::string_view label, const Contract* focal_contract, ArbKind kind,
                                    uint16_t cost_or_proceeds, uint8_t legs,
                                    int raw_edge_thou, int net_edge_thou, Size size) {
        if (size == 0 || out.opportunity_count >= out.opportunities.size()) return;

        ArbOpportunity& opp = out.opportunities[out.opportunity_count++];
        opp = {};
        opp.t2_book_updated_ns = t2_book_updated;
        opp.t3_arb_checked_ns = now_ns();
        opp.contract_name = label;
        opp.arb_kind = kind;
        opp.cost_or_proceeds = cost_or_proceeds;
        opp.raw_edge_bps = static_cast<int16_t>(thousandths_to_bps(raw_edge_thou));
        opp.edge_bps = static_cast<int16_t>(thousandths_to_bps(net_edge_thou));
        opp.size_shares = size;
        opp.leg_count = legs;
        opp.theoretical_pnl = static_cast<double>(net_edge_thou) / 1000.0 * size;

        if (focal_contract != nullptr) {
            opp.ask_yes = focal_contract->book_yes.best_ask;
            opp.ask_no = focal_contract->book_no.best_ask;
            opp.bid_yes = focal_contract->book_yes.best_bid;
            opp.bid_no = focal_contract->book_no.best_bid;
        }

        ++total_opps_;
        switch (kind) {
            case ArbKind::BUY_GROUP_YES:
                ++buy_group_;
                break;
            case ArbKind::SELL_GROUP_YES:
                ++sell_group_;
                break;
            case ArbKind::BUY_NO_SELL_OTHERS:
                ++buy_no_sell_others_;
                break;
            case ArbKind::SELL_NO_BUY_OTHERS:
                ++sell_no_buy_others_;
                break;
            case ArbKind::BUY_BOTH:
                ++buy_both_;
                break;
            case ArbKind::SELL_BOTH:
                ++sell_both_;
                break;
            case ArbKind::MAKER_BUY_BOTH:
                ++maker_buy_both_;
                break;
            case ArbKind::MAKER_SELL_BOTH:
                ++maker_sell_both_;
                break;
            case ArbKind::MAKER_BUY_GROUP_YES:
                ++maker_buy_group_;
                break;
            case ArbKind::MAKER_SELL_GROUP_YES:
                ++maker_sell_group_;
                break;
            case ArbKind::MAKER_BUY_NO_SELL_OTHERS:
                ++maker_buy_no_sell_others_;
                break;
            case ArbKind::MAKER_SELL_NO_BUY_OTHERS:
                ++maker_sell_no_buy_others_;
                break;
        }
    };

    {
        bool usable = true;
        Size min_size = std::numeric_limits<Size>::max();
        int total_ask = 0;
        int total_fee = 0;

        for (const Contract* contract : group.contracts) {
            const Orderbook& book = contract->book_yes;
            if (!book.has_snapshot || book.best_ask == 0 || book.best_ask_size == 0) {
                usable = false;
                break;
            }
            total_ask += static_cast<int>(book.best_ask);
            total_fee += per_share_fee_thou(*contract, book.best_ask);
            min_size = std::min(min_size, book.best_ask_size);
        }

        if (usable && min_size != std::numeric_limits<Size>::max()) {
            ++total_checks_;
            ++out.checks_performed;

            const int raw_edge_thou = static_cast<int>(PRICE_ONE) - total_ask;
            const int net_edge_thou = raw_edge_thou - total_fee;
            record_edge_sample(out, t2_book_updated, group.display_name, ArbKind::BUY_GROUP_YES,
                               static_cast<uint16_t>(total_ask), leg_count, raw_edge_thou, net_edge_thou);

            if (net_edge_thou > min_edge_thou) {
                emit_group_opp(group.display_name, nullptr, ArbKind::BUY_GROUP_YES,
                               static_cast<uint16_t>(total_ask), leg_count,
                               raw_edge_thou, net_edge_thou, min_size);
            }
        }
    }

    {
        bool usable = true;
        Size min_size = std::numeric_limits<Size>::max();
        int total_bid = 0;
        int total_fee = 0;

        for (const Contract* contract : group.contracts) {
            const Orderbook& book = contract->book_yes;
            if (!book.has_snapshot || book.best_bid == 0 || book.best_bid_size == 0) {
                usable = false;
                break;
            }
            total_bid += static_cast<int>(book.best_bid);
            total_fee += per_share_fee_thou(*contract, book.best_bid);
            min_size = std::min(min_size, book.best_bid_size);
        }

        if (usable && min_size != std::numeric_limits<Size>::max()) {
            ++total_checks_;
            ++out.checks_performed;

            const int raw_edge_thou = total_bid - static_cast<int>(PRICE_ONE);
            const int net_edge_thou = raw_edge_thou - total_fee;
            record_edge_sample(out, t2_book_updated, group.display_name, ArbKind::SELL_GROUP_YES,
                               static_cast<uint16_t>(total_bid), leg_count, raw_edge_thou, net_edge_thou);

            if (net_edge_thou > min_edge_thou) {
                emit_group_opp(group.display_name, nullptr, ArbKind::SELL_GROUP_YES,
                               static_cast<uint16_t>(total_bid), leg_count,
                               raw_edge_thou, net_edge_thou, min_size);
            }
        }
    }

    {
        BestFocalOpportunity best_buy_no_sell_others;
        BestFocalOpportunity best_sell_no_buy_others;

        for (const Contract* focal : group.contracts) {
            {
                bool usable = focal->book_no.has_snapshot &&
                              focal->book_no.best_ask > 0 &&
                              focal->book_no.best_ask_size > 0;
                Size min_size = focal->book_no.best_ask_size;
                int basket_proceeds = 0;
                int fee_cost = 0;

                if (usable) {
                    fee_cost += per_share_fee_thou(*focal, focal->book_no.best_ask);
                    for (const Contract* other : group.contracts) {
                        if (other == focal) continue;
                        if (!other->book_yes.has_snapshot ||
                            other->book_yes.best_bid == 0 ||
                            other->book_yes.best_bid_size == 0) {
                            usable = false;
                            break;
                        }
                        basket_proceeds += static_cast<int>(other->book_yes.best_bid);
                        fee_cost += per_share_fee_thou(*other, other->book_yes.best_bid);
                        min_size = std::min(min_size, other->book_yes.best_bid_size);
                    }
                }

                if (usable) {
                    ++total_checks_;
                    ++out.checks_performed;

                    const int raw_edge_thou =
                        basket_proceeds - static_cast<int>(focal->book_no.best_ask);
                    const int net_edge_thou = raw_edge_thou - fee_cost;
                    const uint16_t reference_value = static_cast<uint16_t>(basket_proceeds);
                    record_edge_sample(out, t2_book_updated, focal->asset_name, ArbKind::BUY_NO_SELL_OTHERS,
                                       reference_value, leg_count, raw_edge_thou, net_edge_thou);
                    best_buy_no_sell_others.consider(focal, reference_value, raw_edge_thou,
                                                     net_edge_thou, min_size);
                }
            }

            {
                bool usable = focal->book_no.has_snapshot &&
                              focal->book_no.best_bid > 0 &&
                              focal->book_no.best_bid_size > 0;
                Size min_size = focal->book_no.best_bid_size;
                int basket_cost = 0;
                int fee_cost = 0;

                if (usable) {
                    fee_cost += per_share_fee_thou(*focal, focal->book_no.best_bid);
                    for (const Contract* other : group.contracts) {
                        if (other == focal) continue;
                        if (!other->book_yes.has_snapshot ||
                            other->book_yes.best_ask == 0 ||
                            other->book_yes.best_ask_size == 0) {
                            usable = false;
                            break;
                        }
                        basket_cost += static_cast<int>(other->book_yes.best_ask);
                        fee_cost += per_share_fee_thou(*other, other->book_yes.best_ask);
                        min_size = std::min(min_size, other->book_yes.best_ask_size);
                    }
                }

                if (usable) {
                    ++total_checks_;
                    ++out.checks_performed;

                    const int raw_edge_thou =
                        static_cast<int>(focal->book_no.best_bid) - basket_cost;
                    const int net_edge_thou = raw_edge_thou - fee_cost;
                    const uint16_t reference_value = static_cast<uint16_t>(basket_cost);
                    record_edge_sample(out, t2_book_updated, focal->asset_name, ArbKind::SELL_NO_BUY_OTHERS,
                                       reference_value, leg_count, raw_edge_thou, net_edge_thou);
                    best_sell_no_buy_others.consider(focal, reference_value, raw_edge_thou,
                                                     net_edge_thou, min_size);
                }
            }
        }

        if (best_buy_no_sell_others.focal != nullptr &&
            best_buy_no_sell_others.net_edge_thou > min_edge_thou) {
            emit_group_opp(best_buy_no_sell_others.focal->asset_name,
                           best_buy_no_sell_others.focal,
                           ArbKind::BUY_NO_SELL_OTHERS,
                           best_buy_no_sell_others.reference_value,
                           leg_count,
                           best_buy_no_sell_others.raw_edge_thou,
                           best_buy_no_sell_others.net_edge_thou,
                           best_buy_no_sell_others.size_shares);
        }

        if (best_sell_no_buy_others.focal != nullptr &&
            best_sell_no_buy_others.net_edge_thou > min_edge_thou) {
            emit_group_opp(best_sell_no_buy_others.focal->asset_name,
                           best_sell_no_buy_others.focal,
                           ArbKind::SELL_NO_BUY_OTHERS,
                           best_sell_no_buy_others.reference_value,
                           leg_count,
                           best_sell_no_buy_others.raw_edge_thou,
                           best_sell_no_buy_others.net_edge_thou,
                           best_sell_no_buy_others.size_shares);
        }
    }

    if (!config_.maker_arb_enabled) {
        return out;
    }

    {
        int best_raw_edge_thou = std::numeric_limits<int>::min();
        int best_net_edge_thou = std::numeric_limits<int>::min();
        uint16_t best_cost = 0;
        Size best_size = 0;

        for (const Contract* passive : group.contracts) {
            const Price maker_buy = inside_buy_price(passive->book_yes);
            if (maker_buy == 0) continue;

            bool usable = true;
            Size min_size = std::numeric_limits<Size>::max();
            int total_cost = 0;
            int total_fee = 0;

            for (const Contract* contract : group.contracts) {
                const Orderbook& book = contract->book_yes;
                if (!book.has_snapshot) {
                    usable = false;
                    break;
                }
                const Price price = (contract == passive) ? maker_buy : book.best_ask;
                if (price == 0) {
                    usable = false;
                    break;
                }
                total_cost += static_cast<int>(price);
                total_fee += per_share_fee_thou(*contract, price);
                if (contract != passive) {
                    if (book.best_ask_size == 0) {
                        usable = false;
                        break;
                    }
                    min_size = std::min(min_size, book.best_ask_size);
                }
            }

            if (!usable || min_size == std::numeric_limits<Size>::max()) continue;

            const int raw_edge_thou = static_cast<int>(PRICE_ONE) - total_cost;
            const int net_edge_thou = raw_edge_thou - total_fee;
            if (net_edge_thou > best_net_edge_thou) {
                best_raw_edge_thou = raw_edge_thou;
                best_net_edge_thou = net_edge_thou;
                best_cost = static_cast<uint16_t>(total_cost);
                best_size = min_size;
            }
        }

        if (best_net_edge_thou != std::numeric_limits<int>::min()) {
            ++total_checks_;
            ++out.checks_performed;
            record_edge_sample(out, t2_book_updated, group.display_name, ArbKind::MAKER_BUY_GROUP_YES,
                               best_cost, leg_count, best_raw_edge_thou, best_net_edge_thou);
            if (best_net_edge_thou > min_edge_thou) {
                emit_group_opp(group.display_name, nullptr, ArbKind::MAKER_BUY_GROUP_YES,
                               best_cost, leg_count, best_raw_edge_thou, best_net_edge_thou, best_size);
            }
        }
    }

    {
        int best_raw_edge_thou = std::numeric_limits<int>::min();
        int best_net_edge_thou = std::numeric_limits<int>::min();
        uint16_t best_proceeds = 0;
        Size best_size = 0;

        for (const Contract* passive : group.contracts) {
            const Price maker_sell = inside_sell_price(passive->book_yes);
            if (maker_sell == 0) continue;

            bool usable = true;
            Size min_size = std::numeric_limits<Size>::max();
            int total_proceeds = 0;
            int total_fee = 0;

            for (const Contract* contract : group.contracts) {
                const Orderbook& book = contract->book_yes;
                if (!book.has_snapshot) {
                    usable = false;
                    break;
                }
                const Price price = (contract == passive) ? maker_sell : book.best_bid;
                if (price == 0) {
                    usable = false;
                    break;
                }
                total_proceeds += static_cast<int>(price);
                total_fee += per_share_fee_thou(*contract, price);
                if (contract != passive) {
                    if (book.best_bid_size == 0) {
                        usable = false;
                        break;
                    }
                    min_size = std::min(min_size, book.best_bid_size);
                }
            }

            if (!usable || min_size == std::numeric_limits<Size>::max()) continue;

            const int raw_edge_thou = total_proceeds - static_cast<int>(PRICE_ONE);
            const int net_edge_thou = raw_edge_thou - total_fee;
            if (net_edge_thou > best_net_edge_thou) {
                best_raw_edge_thou = raw_edge_thou;
                best_net_edge_thou = net_edge_thou;
                best_proceeds = static_cast<uint16_t>(total_proceeds);
                best_size = min_size;
            }
        }

        if (best_net_edge_thou != std::numeric_limits<int>::min()) {
            ++total_checks_;
            ++out.checks_performed;
            record_edge_sample(out, t2_book_updated, group.display_name, ArbKind::MAKER_SELL_GROUP_YES,
                               best_proceeds, leg_count, best_raw_edge_thou, best_net_edge_thou);
            if (best_net_edge_thou > min_edge_thou) {
                emit_group_opp(group.display_name, nullptr, ArbKind::MAKER_SELL_GROUP_YES,
                               best_proceeds, leg_count, best_raw_edge_thou, best_net_edge_thou, best_size);
            }
        }
    }

    {
        BestFocalOpportunity best_maker_buy_no_sell_others;
        BestFocalOpportunity best_maker_sell_no_buy_others;

        for (const Contract* focal : group.contracts) {
            {
                int best_raw_edge_thou = std::numeric_limits<int>::min();
                int best_net_edge_thou = std::numeric_limits<int>::min();
                uint16_t best_reference = 0;
                Size best_size = 0;

                const Price focal_inside = inside_buy_price(focal->book_no);
                if (focal_inside > 0) {
                    bool usable = true;
                    Size min_size = std::numeric_limits<Size>::max();
                    int basket_proceeds = 0;
                    int fee_cost = per_share_fee_thou(*focal, focal_inside);

                    for (const Contract* other : group.contracts) {
                        if (other == focal) continue;
                        if (!other->book_yes.has_snapshot ||
                            other->book_yes.best_bid == 0 ||
                            other->book_yes.best_bid_size == 0) {
                            usable = false;
                            break;
                        }
                        basket_proceeds += static_cast<int>(other->book_yes.best_bid);
                        fee_cost += per_share_fee_thou(*other, other->book_yes.best_bid);
                        min_size = std::min(min_size, other->book_yes.best_bid_size);
                    }

                    if (usable && min_size != std::numeric_limits<Size>::max()) {
                        const int raw_edge_thou = basket_proceeds - static_cast<int>(focal_inside);
                        const int net_edge_thou = raw_edge_thou - fee_cost;
                        best_raw_edge_thou = raw_edge_thou;
                        best_net_edge_thou = net_edge_thou;
                        best_reference = static_cast<uint16_t>(basket_proceeds);
                        best_size = min_size;
                    }
                }

                if (focal->book_no.has_snapshot && focal->book_no.best_ask > 0 &&
                    focal->book_no.best_ask_size > 0) {
                    for (const Contract* passive : group.contracts) {
                        if (passive == focal) continue;
                        const Price maker_sell = inside_sell_price(passive->book_yes);
                        if (maker_sell == 0) continue;

                        bool usable = true;
                        Size min_size = focal->book_no.best_ask_size;
                        int basket_proceeds = 0;
                        int fee_cost = per_share_fee_thou(*focal, focal->book_no.best_ask);

                        for (const Contract* other : group.contracts) {
                            if (other == focal) continue;
                            if (!other->book_yes.has_snapshot) {
                                usable = false;
                                break;
                            }
                            const Price price = (other == passive) ? maker_sell : other->book_yes.best_bid;
                            if (price == 0) {
                                usable = false;
                                break;
                            }
                            basket_proceeds += static_cast<int>(price);
                            fee_cost += per_share_fee_thou(*other, price);
                            if (other != passive) {
                                if (other->book_yes.best_bid_size == 0) {
                                    usable = false;
                                    break;
                                }
                                min_size = std::min(min_size, other->book_yes.best_bid_size);
                            }
                        }

                        if (!usable || min_size == 0) continue;

                        const int raw_edge_thou =
                            basket_proceeds - static_cast<int>(focal->book_no.best_ask);
                        const int net_edge_thou = raw_edge_thou - fee_cost;
                        if (net_edge_thou > best_net_edge_thou) {
                            best_raw_edge_thou = raw_edge_thou;
                            best_net_edge_thou = net_edge_thou;
                            best_reference = static_cast<uint16_t>(basket_proceeds);
                            best_size = min_size;
                        }
                    }
                }

                if (best_net_edge_thou != std::numeric_limits<int>::min()) {
                    ++total_checks_;
                    ++out.checks_performed;
                    record_edge_sample(out, t2_book_updated, focal->asset_name,
                                       ArbKind::MAKER_BUY_NO_SELL_OTHERS, best_reference, leg_count,
                                       best_raw_edge_thou, best_net_edge_thou);
                    best_maker_buy_no_sell_others.consider(focal, best_reference, best_raw_edge_thou,
                                                           best_net_edge_thou, best_size);
                }
            }

            {
                int best_raw_edge_thou = std::numeric_limits<int>::min();
                int best_net_edge_thou = std::numeric_limits<int>::min();
                uint16_t best_reference = 0;
                Size best_size = 0;

                const Price focal_inside = inside_sell_price(focal->book_no);
                if (focal_inside > 0) {
                    bool usable = true;
                    Size min_size = std::numeric_limits<Size>::max();
                    int basket_cost = 0;
                    int fee_cost = per_share_fee_thou(*focal, focal_inside);

                    for (const Contract* other : group.contracts) {
                        if (other == focal) continue;
                        if (!other->book_yes.has_snapshot ||
                            other->book_yes.best_ask == 0 ||
                            other->book_yes.best_ask_size == 0) {
                            usable = false;
                            break;
                        }
                        basket_cost += static_cast<int>(other->book_yes.best_ask);
                        fee_cost += per_share_fee_thou(*other, other->book_yes.best_ask);
                        min_size = std::min(min_size, other->book_yes.best_ask_size);
                    }

                    if (usable && min_size != std::numeric_limits<Size>::max()) {
                        const int raw_edge_thou = static_cast<int>(focal_inside) - basket_cost;
                        const int net_edge_thou = raw_edge_thou - fee_cost;
                        best_raw_edge_thou = raw_edge_thou;
                        best_net_edge_thou = net_edge_thou;
                        best_reference = static_cast<uint16_t>(basket_cost);
                        best_size = min_size;
                    }
                }

                if (focal->book_no.has_snapshot && focal->book_no.best_bid > 0 &&
                    focal->book_no.best_bid_size > 0) {
                    for (const Contract* passive : group.contracts) {
                        if (passive == focal) continue;
                        const Price maker_buy = inside_buy_price(passive->book_yes);
                        if (maker_buy == 0) continue;

                        bool usable = true;
                        Size min_size = focal->book_no.best_bid_size;
                        int basket_cost = 0;
                        int fee_cost = per_share_fee_thou(*focal, focal->book_no.best_bid);

                        for (const Contract* other : group.contracts) {
                            if (other == focal) continue;
                            if (!other->book_yes.has_snapshot) {
                                usable = false;
                                break;
                            }
                            const Price price = (other == passive) ? maker_buy : other->book_yes.best_ask;
                            if (price == 0) {
                                usable = false;
                                break;
                            }
                            basket_cost += static_cast<int>(price);
                            fee_cost += per_share_fee_thou(*other, price);
                            if (other != passive) {
                                if (other->book_yes.best_ask_size == 0) {
                                    usable = false;
                                    break;
                                }
                                min_size = std::min(min_size, other->book_yes.best_ask_size);
                            }
                        }

                        if (!usable || min_size == 0) continue;

                        const int raw_edge_thou =
                            static_cast<int>(focal->book_no.best_bid) - basket_cost;
                        const int net_edge_thou = raw_edge_thou - fee_cost;
                        if (net_edge_thou > best_net_edge_thou) {
                            best_raw_edge_thou = raw_edge_thou;
                            best_net_edge_thou = net_edge_thou;
                            best_reference = static_cast<uint16_t>(basket_cost);
                            best_size = min_size;
                        }
                    }
                }

                if (best_net_edge_thou != std::numeric_limits<int>::min()) {
                    ++total_checks_;
                    ++out.checks_performed;
                    record_edge_sample(out, t2_book_updated, focal->asset_name,
                                       ArbKind::MAKER_SELL_NO_BUY_OTHERS, best_reference, leg_count,
                                       best_raw_edge_thou, best_net_edge_thou);
                    best_maker_sell_no_buy_others.consider(focal, best_reference, best_raw_edge_thou,
                                                           best_net_edge_thou, best_size);
                }
            }
        }

        if (best_maker_buy_no_sell_others.focal != nullptr &&
            best_maker_buy_no_sell_others.net_edge_thou > min_edge_thou) {
            emit_group_opp(best_maker_buy_no_sell_others.focal->asset_name,
                           best_maker_buy_no_sell_others.focal,
                           ArbKind::MAKER_BUY_NO_SELL_OTHERS,
                           best_maker_buy_no_sell_others.reference_value,
                           leg_count,
                           best_maker_buy_no_sell_others.raw_edge_thou,
                           best_maker_buy_no_sell_others.net_edge_thou,
                           best_maker_buy_no_sell_others.size_shares);
        }

        if (best_maker_sell_no_buy_others.focal != nullptr &&
            best_maker_sell_no_buy_others.net_edge_thou > min_edge_thou) {
            emit_group_opp(best_maker_sell_no_buy_others.focal->asset_name,
                           best_maker_sell_no_buy_others.focal,
                           ArbKind::MAKER_SELL_NO_BUY_OTHERS,
                           best_maker_sell_no_buy_others.reference_value,
                           leg_count,
                           best_maker_sell_no_buy_others.raw_edge_thou,
                           best_maker_sell_no_buy_others.net_edge_thou,
                           best_maker_sell_no_buy_others.size_shares);
        }
    }

    return out;
}
