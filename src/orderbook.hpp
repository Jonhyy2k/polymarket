#pragma once

#include "types.hpp"
#include "parser.hpp"
#include <simdjson.h>

class OrderbookManager {
public:
    // Full update: parse bids and asks arrays from the JSON object into the orderbook
    static void full_update(Orderbook& book, simdjson::ondemand::object& obj);

    // Incremental top-of-book update from market price_change event
    static void apply_price_change(Orderbook& book, const ParsedPriceChangeEvent& ev);

    // Ultra-light top-of-book refresh when custom_feature_enabled is enabled
    static void apply_best_bid_ask(Orderbook& book, const ParsedBestBidAskEvent& ev);

    // Print orderbook summary
    static void print_summary(const Orderbook& book, const char* label);
};
