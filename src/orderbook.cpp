#include "orderbook.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace {

inline uint32_t advance_epoch(uint32_t epoch, uint32_t (&epoch_by_price)[PRICE_LEVEL_COUNT]) noexcept {
    ++epoch;
    if (epoch == 0) {
        std::memset(epoch_by_price, 0, sizeof(epoch_by_price));
        epoch = 1;
    }
    return epoch;
}

inline Size bid_size_at(const Orderbook& book, Price price) noexcept {
    return (price > 0 && price <= PRICE_ONE && book.bid_epoch_by_price[price] == book.bid_epoch)
        ? book.bid_size_by_price[price]
        : 0;
}

inline Size ask_size_at(const Orderbook& book, Price price) noexcept {
    return (price > 0 && price <= PRICE_ONE && book.ask_epoch_by_price[price] == book.ask_epoch)
        ? book.ask_size_by_price[price]
        : 0;
}

inline void occ_set(uint64_t* occ, Price price, bool present) noexcept {
    const uint64_t bit = uint64_t{1} << (price & 63);
    if (present) occ[price >> 6] |= bit;
    else         occ[price >> 6] &= ~bit;
}

inline void set_bid_level(Orderbook& book, Price price, Size size) noexcept {
    book.bid_size_by_price[price] = size;
    book.bid_epoch_by_price[price] = book.bid_epoch;
    occ_set(book.bid_occ, price, size > 0);
}

inline void set_ask_level(Orderbook& book, Price price, Size size) noexcept {
    book.ask_size_by_price[price] = size;
    book.ask_epoch_by_price[price] = book.ask_epoch;
    occ_set(book.ask_occ, price, size > 0);
}

// Highest occupied bid = highest set bit across the occupancy words (high word
// first). __builtin_clzll gives the leading zeros, so 63-clz is the top bit.
inline Price find_best_bid(const Orderbook& book) noexcept {
    for (size_t w = PRICE_OCC_WORDS; w-- > 0; ) {
        const uint64_t word = book.bid_occ[w];
        if (word) {
            return static_cast<Price>(w * 64 + (63 - __builtin_clzll(word)));
        }
    }
    return 0;
}

// Lowest occupied ask = lowest set bit (low word first); __builtin_ctzll gives
// the trailing-zero count = index of the lowest set bit.
inline Price find_best_ask(const Orderbook& book) noexcept {
    for (size_t w = 0; w < PRICE_OCC_WORDS; ++w) {
        const uint64_t word = book.ask_occ[w];
        if (word) {
            return static_cast<Price>(w * 64 + __builtin_ctzll(word));
        }
    }
    return 0;
}

inline void set_best_from_ladders(Orderbook& book) noexcept {
    book.best_bid = find_best_bid(book);
    book.best_ask = find_best_ask(book);
    book.best_bid_size = (book.best_bid > 0) ? bid_size_at(book, book.best_bid) : 0;
    book.best_ask_size = (book.best_ask > 0) ? ask_size_at(book, book.best_ask) : 0;
}

// CLOB v2 best_bid_ask events carry only prices (best_bid/best_ask/spread), NO
// sizes, so we cannot set a level's depth from the hint. When the hinted price
// isn't in our dense ladder the book is momentarily behind; we fall back to the
// ladder-derived best (which always has a real size) rather than inventing one.
// The fresh best also arrives via price_change (which DOES carry size and
// updates the ladder), so this path is effectively a redundant safety refresh.
inline void set_best_from_bbo_hints(Orderbook& book, Price hinted_bid, Price hinted_ask) noexcept {
    const Size hinted_bid_size = bid_size_at(book, hinted_bid);
    if (hinted_bid > 0 && hinted_bid_size > 0) {
        book.best_bid = hinted_bid;
        book.best_bid_size = hinted_bid_size;
    } else {
        book.best_bid = find_best_bid(book);
        book.best_bid_size = (book.best_bid > 0) ? bid_size_at(book, book.best_bid) : 0;
    }

    const Size hinted_ask_size = ask_size_at(book, hinted_ask);
    if (hinted_ask > 0 && hinted_ask_size > 0) {
        book.best_ask = hinted_ask;
        book.best_ask_size = hinted_ask_size;
    } else {
        book.best_ask = find_best_ask(book);
        book.best_ask_size = (book.best_ask > 0) ? ask_size_at(book, book.best_ask) : 0;
    }
}

}  // namespace

void OrderbookManager::full_update(Orderbook& book, simdjson::ondemand::object& obj) {
    book.bid_epoch = advance_epoch(book.bid_epoch, book.bid_epoch_by_price);
    book.ask_epoch = advance_epoch(book.ask_epoch, book.ask_epoch_by_price);
    std::fill(std::begin(book.bids), std::end(book.bids), PriceLevel{});
    std::fill(std::begin(book.asks), std::end(book.asks), PriceLevel{});
    // The occupancy bitmap cannot be lazily epoch-invalidated like the dense
    // size array, so a snapshot must clear it and re-set only the new levels.
    std::memset(book.bid_occ, 0, sizeof(book.bid_occ));
    std::memset(book.ask_occ, 0, sizeof(book.ask_occ));

    // Parse bids
    book.bid_count = 0;

    simdjson::ondemand::array bids;
    if (!obj["bids"].get_array().get(bids)) {
        for (auto level : bids) {
            simdjson::ondemand::object lobj;
            if (level.get_object().get(lobj)) continue;

            std::string_view price_str, size_str;
            if (lobj["price"].get_string().get(price_str)) continue;
            if (lobj["size"].get_string().get(size_str)) continue;

            Price p = 0;
            Size  s = 0;
            if (!try_parse_price(price_str, p) || !try_parse_size(size_str, s) || p == 0 || s == 0) continue;

            // Populate the dense ladder for EVERY level so find_best_bid sees the
            // true best. Polymarket sends bids ASCENDING (best/highest last) and
            // books can exceed MAX_LEVELS, so capping the parse loop here would
            // drop the best bid. The bids[] display array is debug-only (its
            // contents are never read), so we cap only its fill, not the ladder.
            set_bid_level(book, p, s);
            if (book.bid_count < MAX_LEVELS) {
                book.bids[book.bid_count] = {p, s};
                book.bid_count++;
            }
        }
    }

    // Parse asks
    book.ask_count = 0;

    simdjson::ondemand::array asks;
    if (!obj["asks"].get_array().get(asks)) {
        for (auto level : asks) {
            simdjson::ondemand::object lobj;
            if (level.get_object().get(lobj)) continue;

            std::string_view price_str, size_str;
            if (lobj["price"].get_string().get(price_str)) continue;
            if (lobj["size"].get_string().get(size_str)) continue;

            Price p = 0;
            Size  s = 0;
            if (!try_parse_price(price_str, p) || !try_parse_size(size_str, s) || p == 0 || s == 0) continue;

            set_ask_level(book, p, s);
            if (book.ask_count < MAX_LEVELS) {
                book.asks[book.ask_count] = {p, s};
                book.ask_count++;
            }
        }
    }

    set_best_from_ladders(book);
    book.has_snapshot = true;
}

void OrderbookManager::apply_price_change(Orderbook& book, const ParsedPriceChangeEvent& ev) {
    if (ev.price == 0 || ev.price > PRICE_ONE) return;

    if (ev.is_bid) {
        set_bid_level(book, ev.price, ev.size);
    } else {
        set_ask_level(book, ev.price, ev.size);
    }

    set_best_from_ladders(book);
}

void OrderbookManager::apply_best_bid_ask(Orderbook& book, const ParsedBestBidAskEvent& ev) {
    set_best_from_bbo_hints(book, ev.best_bid, ev.best_ask);
}

void OrderbookManager::print_summary(const Orderbook& book, const char* label) {
    double parse_us = 0;  // caller can compute if needed
    printf("[BOOK] %-3s | bid=%u(%u) ask=%u(%u) spread=%d | levels=%u/%u\n",
           label,
           book.best_bid, book.best_bid_size,
           book.best_ask, book.best_ask_size,
           (int)book.best_ask - (int)book.best_bid,
           book.bid_count, book.ask_count);
}
