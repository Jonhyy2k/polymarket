#include "oms.hpp"

#include <cmath>
#include <cstdio>

bool ShadowGateway::submit(const ManagedOrder& order) {
    ++submitted_;
    if (verbose_) {
        std::printf("[SHADOW-OMS] CREATE  cid=%llu %-4s %.3f x %u  %.40s\n",
                    static_cast<unsigned long long>(order.client_id),
                    order_side_name(order.side),
                    static_cast<double>(order.price) / 1000.0, order.size,
                    order.token_id.c_str());
    }
    return true;
}

bool ShadowGateway::cancel(const ManagedOrder& order) {
    ++canceled_;
    if (verbose_) {
        std::printf("[SHADOW-OMS] CANCEL  cid=%llu %-4s %.3f x %u  %.40s\n",
                    static_cast<unsigned long long>(order.client_id),
                    order_side_name(order.side),
                    static_cast<double>(order.price) / 1000.0, order.size,
                    order.token_id.c_str());
    }
    return true;
}

Oms::TokenBook& Oms::book_for(const std::string& token_id) {
    for (auto& kv : books_) {
        if (kv.first == token_id) return kv.second;
    }
    books_.emplace_back(token_id, TokenBook{});
    return books_.back().second;
}

const Oms::TokenBook* Oms::find_book(const std::string& token_id) const {
    for (const auto& kv : books_) {
        if (kv.first == token_id) return &kv.second;
    }
    return nullptr;
}

size_t Oms::open_order_count() const {
    size_t n = 0;
    for (const auto& kv : books_) {
        n += kv.second.has_bid ? 1 : 0;
        n += kv.second.has_ask ? 1 : 0;
    }
    return n;
}

double Oms::net_position(const std::string& token_id) const {
    const TokenBook* tb = find_book(token_id);
    return tb ? tb->net_position : 0.0;
}

bool Oms::passes_risk(const std::string& token_id, const RewardQuote& q, OrderSide side) {
    if (limits_.kill_switch) return false;
    if (open_order_count() >= limits_.max_open_orders_total) return false;

    // Gross notional across all live orders + this candidate.
    double gross = q.size ? (static_cast<double>(q.price) / static_cast<double>(PRICE_ONE) *
                             static_cast<double>(q.size)) : 0.0;
    for (const auto& kv : books_) {
        if (kv.second.has_bid) gross += kv.second.bid.notional_usd();
        if (kv.second.has_ask) gross += kv.second.ask.notional_usd();
    }
    if (gross > limits_.max_gross_notional_usd) return false;

    // Don't add inventory in the direction that breaches the position cap.
    const TokenBook* tb = find_book(token_id);
    const double pos = tb ? tb->net_position : 0.0;
    const double signed_add = (side == OrderSide::BUY ? 1.0 : -1.0) * static_cast<double>(q.size);
    if (std::abs(pos + signed_add) > limits_.max_position_shares) return false;

    return true;
}

void Oms::place(TokenBook& tb, const std::string& token_id, const RewardQuote& q,
                OrderSide side, uint32_t mid2) {
    ManagedOrder o{};
    o.client_id = next_client_id_++;
    o.token_id = token_id;
    o.side = side;
    o.price = q.price;
    o.size = q.size;
    o.state = OrderState::Submitted;
    o.created_ns = now_ns();
    o.ref_mid2 = mid2;
    if (!gateway_.submit(o)) {
        return;  // gateway refused; leave the side empty (will retry next reconcile)
    }
    o.state = OrderState::Live;  // shadow gateway acks synchronously
    ++stats_.creates;
    if (side == OrderSide::BUY) { tb.bid = o; tb.has_bid = true; }
    else                        { tb.ask = o; tb.has_ask = true; }
}

void Oms::drop(ManagedOrder& order, bool& has_flag) {
    if (!has_flag) return;
    gateway_.cancel(order);
    ++stats_.cancels;
    order.state = OrderState::Canceled;
    has_flag = false;
}

void Oms::reconcile(const std::string& token_id, const DesiredQuotes& desired, uint32_t mid2) {
    TokenBook& tb = book_for(token_id);

    // ---- BID side ----
    if (!desired.bid.valid) {
        drop(tb.bid, tb.has_bid);
    } else if (tb.has_bid && tb.bid.price == desired.bid.price && tb.bid.size == desired.bid.size) {
        // identical -> leave resting (idempotent, preserves queue priority)
    } else {
        if (tb.has_bid) { drop(tb.bid, tb.has_bid); ++stats_.replaces; }
        if (passes_risk(token_id, desired.bid, OrderSide::BUY)) {
            place(tb, token_id, desired.bid, OrderSide::BUY, mid2);
        } else {
            ++stats_.risk_rejects;
        }
    }

    // ---- ASK side ----
    if (!desired.ask.valid) {
        drop(tb.ask, tb.has_ask);
    } else if (tb.has_ask && tb.ask.price == desired.ask.price && tb.ask.size == desired.ask.size) {
        // identical -> leave resting
    } else {
        if (tb.has_ask) { drop(tb.ask, tb.has_ask); ++stats_.replaces; }
        if (passes_risk(token_id, desired.ask, OrderSide::SELL)) {
            place(tb, token_id, desired.ask, OrderSide::SELL, mid2);
        } else {
            ++stats_.risk_rejects;
        }
    }
}

LiveSide Oms::live_side(const std::string& token_id) const {
    LiveSide ls;
    const TokenBook* tb = find_book(token_id);
    if (!tb) return ls;
    if (tb->has_bid) ls.bid = {true, tb->bid.price, tb->bid.ref_mid2, tb->bid.size, tb->bid.client_id};
    if (tb->has_ask) ls.ask = {true, tb->ask.price, tb->ask.ref_mid2, tb->ask.size, tb->ask.client_id};
    return ls;
}

void Oms::cancel_all(const std::string& token_id) {
    TokenBook& tb = book_for(token_id);
    drop(tb.bid, tb.has_bid);
    drop(tb.ask, tb.has_ask);
}

void Oms::cancel_everything() {
    for (auto& kv : books_) {
        drop(kv.second.bid, kv.second.has_bid);
        drop(kv.second.ask, kv.second.has_ask);
    }
}

void Oms::apply_fill(const std::string& token_id, uint64_t client_id, Size fill_size) {
    TokenBook& tb = book_for(token_id);
    ManagedOrder* o = nullptr;
    if (tb.has_bid && tb.bid.client_id == client_id) o = &tb.bid;
    else if (tb.has_ask && tb.ask.client_id == client_id) o = &tb.ask;
    if (!o) return;

    const Size f = std::min(fill_size, o->remaining());
    if (f == 0) return;
    o->filled += f;
    ++stats_.fills;
    stats_.filled_notional_usd += static_cast<double>(o->price) / static_cast<double>(PRICE_ONE) *
                                  static_cast<double>(f);
    tb.net_position += (o->side == OrderSide::BUY ? 1.0 : -1.0) * static_cast<double>(f);

    if (o->remaining() == 0) {
        o->state = OrderState::Filled;
        if (o->side == OrderSide::BUY) tb.has_bid = false; else tb.has_ask = false;
    } else {
        o->state = OrderState::PartiallyFilled;
    }
}
