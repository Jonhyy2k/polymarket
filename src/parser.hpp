#pragma once

#include "types.hpp"
#include <simdjson.h>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

inline bool try_parse_price(std::string_view s, Price& out) noexcept {
    out = 0;
    if (s.empty()) return false;

    uint32_t whole = 0;
    uint32_t frac = 0;
    int frac_digits = 0;
    bool seen_dot = false;
    bool seen_digit = false;

    for (char c : s) {
        if (c == '.') {
            if (seen_dot) return false;
            seen_dot = true;
            continue;
        }
        const unsigned digit = static_cast<unsigned>(c - '0');
        if (digit > 9) return false;
        seen_digit = true;
        if (!seen_dot) {
            if (whole > (std::numeric_limits<uint32_t>::max() - digit) / 10) return false;
            whole = whole * 10 + digit;
        } else if (frac_digits < 3) {
            frac = frac * 10 + digit;
            ++frac_digits;
        }
    }

    if (!seen_digit) return false;

    while (frac_digits < 3) {
        frac *= 10;
        ++frac_digits;
    }

    if (whole > PRICE_ONE / 1000) return false;
    const uint32_t value = whole * 1000 + frac;
    if (value > PRICE_ONE) return false;

    out = static_cast<Price>(value);
    return true;
}

inline Price parse_price(std::string_view s) noexcept {
    Price out = 0;
    return try_parse_price(s, out) ? out : 0;
}

inline bool try_parse_size(std::string_view s, Size& out) noexcept {
    out = 0;
    if (s.empty()) return false;

    bool seen_digit = false;
    bool seen_dot = false;
    for (char c : s) {
        if (c == '.') {
            if (seen_dot) return false;
            seen_dot = true;
            continue;
        }
        const unsigned digit = static_cast<unsigned>(c - '0');
        if (digit > 9) return false;
        if (seen_dot) continue;
        if (out > (std::numeric_limits<Size>::max() - digit) / 10) return false;
        out = out * 10 + digit;
        seen_digit = true;
    }
    return seen_digit;
}

inline Size parse_size(std::string_view s) noexcept {
    Size out = 0;
    return try_parse_size(s, out) ? out : 0;
}

inline bool try_parse_u64(std::string_view s, uint64_t& out) noexcept {
    out = 0;
    if (s.empty()) return false;

    bool seen_digit = false;
    for (char c : s) {
        const unsigned digit = static_cast<unsigned>(c - '0');
        if (digit > 9) return false;
        if (out > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        out = out * 10 + digit;
        seen_digit = true;
    }
    return seen_digit;
}

inline uint64_t parse_u64(std::string_view s) noexcept {
    uint64_t out = 0;
    return try_parse_u64(s, out) ? out : 0;
}

inline bool parse_timestamp_field(simdjson::ondemand::object& obj, const char* field_name,
                                  uint64_t& out) noexcept {
    out = 0;

    simdjson::ondemand::value value;
    if (obj.find_field_unordered(field_name).get(value)) return false;

    simdjson::ondemand::json_type type;
    if (value.type().get(type)) return false;

    if (type == simdjson::ondemand::json_type::number) {
        uint64_t u64 = 0;
        if (!value.get_uint64().get(u64)) {
            out = u64;
            return true;
        }

        int64_t i64 = 0;
        if (!value.get_int64().get(i64) && i64 >= 0) {
            out = static_cast<uint64_t>(i64);
            return true;
        }
        return false;
    }

    if (type == simdjson::ondemand::json_type::string) {
        std::string_view sv;
        if (value.get_string().get(sv)) return false;
        return try_parse_u64(sv, out);
    }

    return false;
}

enum class EventType {
    BOOK,
    LAST_TRADE_PRICE,
    PRICE_CHANGE,
    BEST_BID_ASK,
    TICK_SIZE_CHANGE,
    UNKNOWN
};

// FNV-1a over a hex hash string, used only for cheap gap/dup detection.
inline uint64_t hash_fnv1a(std::string_view s) noexcept {
    uint64_t h = 1469598103934665603ull;
    for (char c : s) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ull; }
    return h;
}

struct ParsedBookEvent {
    std::string_view asset_id;
    std::string_view market;
    uint64_t         timestamp = 0;
    Price            tick_size = 0;   // thousandths; 0 if field absent
    uint64_t         hash = 0;        // FNV-1a of the book hash string (0 if absent)
    bool             valid = false;
};

struct ParsedTickSizeEvent {
    std::string_view asset_id;
    Price            tick_size = 0;
    uint64_t         timestamp = 0;
};

struct ParsedTradeEvent {
    std::string_view asset_id;
    std::string_view price_str;
    std::string_view side;
    std::string_view size_str;
    uint64_t         timestamp = 0;
};

struct ParsedPriceChangeEvent {
    std::string_view asset_id;
    Price            price = 0;
    Size             size = 0;
    Price            best_bid = 0;
    Price            best_ask = 0;
    uint64_t         hash = 0;        // FNV-1a of per-change hash (0 if absent)
    bool             is_bid = false;
    uint64_t         timestamp = 0;
};

struct ParsedBestBidAskEvent {
    std::string_view asset_id;
    Price            best_bid = 0;
    Price            best_ask = 0;
    uint64_t         timestamp = 0;
};

class MessageParser {
public:
    // Frames are parsed in place out of the SPSC ring slot (see parse_padded);
    // 256 KiB comfortably covers the largest observed frame (~48 KiB initial
    // dump, ~5x margin) — oversize frames are dropped + counted, never silently
    // truncated. Sized down from 1 MiB to shrink the message-ring footprint
    // (capacity x slot): 128 x ~256 KiB ≈ 32 MiB instead of ~128 MiB.
    static constexpr size_t kBufferCapacity = 256 * 1024;

    template <typename BookCallback, typename TradeCallback,
              typename PriceChangeCallback, typename BestBidAskCallback,
              typename TickSizeCallback>
    void parse_padded(const char* data, size_t len, size_t allocated,
                      BookCallback&& on_book, TradeCallback&& on_trade,
                      PriceChangeCallback&& on_price_change,
                      BestBidAskCallback&& on_best_bid_ask,
                      TickSizeCallback&& on_tick_size) {
        simdjson::padded_string_view psv(data, len, allocated);
        simdjson::ondemand::document doc;
        auto error = parser_.iterate(psv).get(doc);
        if (error) {
            std::fprintf(stderr, "[PARSE ERROR] %s\n", simdjson::error_message(error));
            return;
        }

        simdjson::ondemand::json_type root_type;
        if (doc.type().get(root_type)) return;

        if (root_type == simdjson::ondemand::json_type::array) {
            simdjson::ondemand::array events;
            if (doc.get_array().get(events)) return;
            for (auto elem : events) {
                simdjson::ondemand::object obj;
                if (elem.get_object().get(obj)) continue;
                parse_event_object(obj, on_book, on_trade, on_price_change, on_best_bid_ask, on_tick_size);
            }
            return;
        }

        if (root_type == simdjson::ondemand::json_type::object) {
            simdjson::ondemand::object obj;
            if (doc.get_object().get(obj)) return;
            parse_event_object(obj, on_book, on_trade, on_price_change, on_best_bid_ask, on_tick_size);
        }
    }

private:
    template <typename BookCallback, typename TradeCallback,
              typename PriceChangeCallback, typename BestBidAskCallback,
              typename TickSizeCallback>
    static void parse_event_object(simdjson::ondemand::object& obj,
                                   BookCallback&& on_book, TradeCallback&& on_trade,
                                   PriceChangeCallback&& on_price_change,
                                   BestBidAskCallback&& on_best_bid_ask,
                                   TickSizeCallback&& on_tick_size) {
        std::string_view event_type;
        if (obj.find_field_unordered("event_type").get_string().get(event_type)) return;

        if (event_type == "book") {
            ParsedBookEvent ev;
            if (obj.find_field_unordered("asset_id").get_string().get(ev.asset_id)) return;
            if (obj.find_field_unordered("market").get_string().get(ev.market)) return;
            parse_timestamp_field(obj, "timestamp", ev.timestamp);
            std::string_view sv;
            if (!obj.find_field_unordered("tick_size").get_string().get(sv)) {
                try_parse_price(sv, ev.tick_size);
            }
            if (!obj.find_field_unordered("hash").get_string().get(sv)) {
                ev.hash = hash_fnv1a(sv);
            }
            ev.valid = true;
            on_book(ev, obj);
            return;
        }

        if (event_type == "tick_size_change") {
            ParsedTickSizeEvent ev;
            if (obj.find_field_unordered("asset_id").get_string().get(ev.asset_id)) return;
            std::string_view sv;
            // V2 emits new_tick_size; fall back to tick_size for forward-compat.
            if (!obj.find_field_unordered("new_tick_size").get_string().get(sv)) {
                try_parse_price(sv, ev.tick_size);
            } else if (!obj.find_field_unordered("tick_size").get_string().get(sv)) {
                try_parse_price(sv, ev.tick_size);
            }
            parse_timestamp_field(obj, "timestamp", ev.timestamp);
            if (ev.tick_size > 0) on_tick_size(ev);
            return;
        }

        if (event_type == "last_trade_price") {
            ParsedTradeEvent ev;
            if (obj.find_field_unordered("asset_id").get_string().get(ev.asset_id)) return;
            if (obj.find_field_unordered("price").get_string().get(ev.price_str)) return;
            if (obj.find_field_unordered("side").get_string().get(ev.side)) return;
            if (obj.find_field_unordered("size").get_string().get(ev.size_str)) return;
            parse_timestamp_field(obj, "timestamp", ev.timestamp);
            on_trade(ev);
            return;
        }

        if (event_type == "price_change") {
            uint64_t timestamp = 0;
            parse_timestamp_field(obj, "timestamp", timestamp);

            simdjson::ondemand::array changes;
            if (obj.find_field_unordered("price_changes").get_array().get(changes)) return;

            for (auto change_elem : changes) {
                simdjson::ondemand::object change;
                if (change_elem.get_object().get(change)) continue;

                ParsedPriceChangeEvent ev;
                std::string_view sv;
                if (change.find_field_unordered("asset_id").get_string().get(ev.asset_id)) continue;
                if (change.find_field_unordered("price").get_string().get(sv)) continue;
                if (!try_parse_price(sv, ev.price)) continue;
                if (change.find_field_unordered("size").get_string().get(sv)) continue;
                if (!try_parse_size(sv, ev.size)) continue;
                if (change.find_field_unordered("side").get_string().get(sv)) continue;
                ev.is_bid = (sv == "BUY");

                if (!change.find_field_unordered("best_bid").get_string().get(sv)) {
                    try_parse_price(sv, ev.best_bid);
                }
                if (!change.find_field_unordered("best_ask").get_string().get(sv)) {
                    try_parse_price(sv, ev.best_ask);
                }
                if (!change.find_field_unordered("hash").get_string().get(sv)) {
                    ev.hash = hash_fnv1a(sv);
                }

                ev.timestamp = timestamp;
                on_price_change(ev);
            }
            return;
        }

        if (event_type == "best_bid_ask") {
            ParsedBestBidAskEvent ev;
            std::string_view sv;
            if (obj.find_field_unordered("asset_id").get_string().get(ev.asset_id)) return;
            if (obj.find_field_unordered("best_bid").get_string().get(sv)) return;
            if (!try_parse_price(sv, ev.best_bid)) return;
            if (obj.find_field_unordered("best_ask").get_string().get(sv)) return;
            if (!try_parse_price(sv, ev.best_ask)) return;
            parse_timestamp_field(obj, "timestamp", ev.timestamp);
            on_best_bid_ask(ev);
        }
    }

    simdjson::ondemand::parser parser_;
};
