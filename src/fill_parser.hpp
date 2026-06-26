#pragma once

// Pure parser for Polymarket user-channel WebSocket messages → FillEvent.
// No I/O, no keys, no state. The LiveGateway subscribes to the (authenticated)
// user channel and feeds each frame here; a valid FillEvent is then mapped from
// exchange_order_id → our client_id and applied via Oms::apply_fill().
//
// SCHEMA CAVEAT: field names follow Polymarket's documented user channel
// (event_type, asset_id, id, size_matched, status, side). Amounts may arrive as
// JSON strings OR numbers — both are handled. Capture a real message and confirm
// field names/units before arming; venues rename fields.

#include <simdjson.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace live {

struct FillEvent {
    std::string event_type;        // "order" | "trade"
    std::string asset_id;          // token id (decimal string) -> ManagedOrder.token_id
    std::string exchange_order_id; // "id" -> ManagedOrder.exchange_order_id
    std::string status;            // "LIVE" | "MATCHED" | "CANCELED" | ...
    std::string side;              // "BUY" | "SELL"
    uint64_t    size_matched = 0;  // CUMULATIVE matched (venue units; reconcile vs order.size)
    bool        valid = false;
};

namespace detail {

inline std::string fp_to_str(simdjson::ondemand::value& v) {
    std::string_view sv;
    if (v.get_string().get(sv)) return {};
    return std::string(sv);
}

inline uint64_t fp_to_u64(simdjson::ondemand::value& v) {
    using namespace simdjson;
    ondemand::json_type t;
    if (v.type().get(t)) return 0;
    if (t == ondemand::json_type::string) {
        std::string_view sv;
        if (v.get_string().get(sv)) return 0;
        return std::strtoull(std::string(sv).c_str(), nullptr, 10);
    }
    if (t == ondemand::json_type::number) {
        double d;
        if (!v.get_double().get(d)) return static_cast<uint64_t>(d);
    }
    return 0;
}

}  // namespace detail

// Parse one user-channel message. valid=true only for recognized order/trade
// updates that carry an asset_id.
inline FillEvent parse_user_message(const std::string& json) {
    using namespace simdjson;
    FillEvent ev;

    ondemand::parser parser;
    padded_string padded(json);
    ondemand::document doc;
    if (parser.iterate(padded).get(doc)) return ev;

    ondemand::object obj;
    if (doc.get_object().get(obj)) return ev;

    for (auto field : obj) {
        std::string_view key;
        if (field.unescaped_key().get(key)) continue;
        ondemand::value val;
        if (field.value().get(val)) continue;

        if      (key == "event_type")   ev.event_type       = detail::fp_to_str(val);
        else if (key == "asset_id")     ev.asset_id          = detail::fp_to_str(val);
        else if (key == "id")           ev.exchange_order_id = detail::fp_to_str(val);
        else if (key == "status")       ev.status            = detail::fp_to_str(val);
        else if (key == "side")         ev.side              = detail::fp_to_str(val);
        else if (key == "size_matched") ev.size_matched      = detail::fp_to_u64(val);
    }

    ev.valid = (ev.event_type == "order" || ev.event_type == "trade")
               && !ev.asset_id.empty();
    return ev;
}

}  // namespace live
