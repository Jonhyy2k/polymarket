# Polymarket Arbitrage Detector — Build Plan

## Project Overview

Build a C++ real-time Polymarket arbitrage detector that connects to the Polymarket WebSocket, maintains a local orderbook, detects YES+NO arbitrage opportunities, and logs virtual paper trades. This is NOT a backtester with historical data — it runs live and tags opportunities in real time without deploying capital.

**Language:** C++17
**Target:** Local development first (Linux/macOS), then deploy to AWS EC2 z1d.3xlarge (eu-west-2b)
**Dependencies:** boost::beast (WebSocket + SSL), simdjson (JSON parsing), OpenSSL
**Build:** CMake

---

## Architecture

```
WebSocket Feed (boost::beast + SSL)
    │
    ├── wss://ws-subscriptions-clob.polymarket.com/ws/market
    │
    ▼
JSON Parser (simdjson, zero-copy from network buffer)
    │
    ▼
Orderbook Manager (pre-allocated, uint16_t prices × 1000)
    │  - Separate book per token (YES bids/asks, NO bids/asks)
    │  - Fixed-size arrays, no heap allocation on hot path
    │
    ▼
Arbitrage Detector
    │  BUY-BOTH:  best_ask_yes + best_ask_no < 1000 (minus fees)
    │  SELL-BOTH: best_bid_yes + best_bid_no > 1000 (plus fees)
    │
    ▼
Paper Trade Logger
    │  - Log every opportunity with full context
    │  - Log every virtual trade (entry, size, theoretical P&L)
    │  - Latency timestamps at every stage (T0-T4)
    │
    ▼
CSV / stdout output (background flush, never on hot path)
```

---

## Polymarket WebSocket Protocol — CRITICAL REFERENCE

### Connection
- **Endpoint:** `wss://ws-subscriptions-clob.polymarket.com/ws/market`
- **TLS:** Required (port 443)
- **Protocol:** Standard WebSocket (RFC 6455) over TLS 1.2+

### Subscription Message (send immediately after WebSocket handshake)
```json
{
    "assets_ids": ["<token_id_yes>", "<token_id_no>"],
    "type": "market"
}
```
- `type` is `"market"` — NOT `"subscribe"`
- `assets_ids` is PLURAL with an array — NOT `"assets_id"` or `"asset_id"`
- No authentication needed for market data channel

### Adding more tokens (after initial subscription)
```json
{
    "assets_ids": ["<new_token_id>"],
    "operation": "subscribe"
}
```

### Incoming Messages — JSON arrays containing event objects

#### Book Event (full orderbook snapshot — PRIMARY DATA SOURCE)
```json
[
    {
        "event_type": "book",
        "asset_id": "65818619657568813474341868652308942079804919287380422192892211131408793125422",
        "market": "0xbd31dc8a20211944f6b70f31557f1001557b59905b7738480ca09bd4532f84af",
        "bids": [
            {"price": ".48", "size": "30"},
            {"price": ".49", "size": "20"},
            {"price": ".50", "size": "15"}
        ],
        "asks": [
            {"price": ".52", "size": "25"},
            {"price": ".53", "size": "60"},
            {"price": ".54", "size": "10"}
        ],
        "timestamp": "123456789000",
        "hash": "0x0...."
    }
]
```
**CRITICAL:** Prices are STRINGS like `".48"` not floats. Sizes are also strings.

#### Last Trade Price Event
```json
[
    {
        "event_type": "last_trade_price",
        "asset_id": "...",
        "market": "0x...",
        "price": "0.456",
        "side": "BUY",
        "size": "219.217767",
        "fee_rate_bps": "0",
        "timestamp": "1750428146322"
    }
]
```

#### Price Change Event (incremental top-of-book update)
Fires when best bid/ask changes. Lighter than full book event.

#### Tick Size Change Event
When price > 0.96 or price < 0.04, tick size changes from 0.01 to 0.001.
```json
[
    {
        "event_type": "tick_size_change",
        "asset_id": "...",
        "old_tick_size": "0.01",
        "new_tick_size": "0.001",
        "timestamp": "100000000"
    }
]
```

### Keepalive
- Respond to WebSocket pings (boost::beast handles this automatically)
- Send your own pings every 15-30 seconds to avoid disconnection

---

## Price Representation

All prices stored as `uint16_t` multiplied by 1000:
- `$0.48` → `480`
- `$0.52` → `520`
- `$1.00` → `1000`
- Range: 10 to 990 (normal tick), down to 1 with 0.001 tick size
- `uint16_t` max is 65535, plenty of headroom

### Price Parser (string → uint16_t)
Polymarket sends prices as strings: `".48"`, `"0.52"`, `".5"`, `"0.456"`
1. Strip leading `"0"` if present
2. Strip the decimal point
3. Pad or truncate to exactly 3 digits
4. Convert to integer

Examples: `".48"` → `480`, `"0.5"` → `500`, `"0.456"` → `456`

Sizes: parse as `uint32_t` (whole dollars, ignore sub-cent fractional sizes for now)

---

## Arbitrage Logic

### BUY-BOTH Arbitrage
```
IF best_ask_yes + best_ask_no < 1000:
    edge_bps = 1000 - (best_ask_yes + best_ask_no)  // in tenths of cents
    executable_size = min(best_ask_size_yes, best_ask_size_no)
    IF edge_bps > MIN_EDGE_THRESHOLD:
        LOG OPPORTUNITY → BUY_BOTH
```
Execution: Buy YES at ask + Buy NO at ask → hold both → guaranteed $1.00 at resolution (or merge via CTF immediately).

### SELL-BOTH Arbitrage
```
IF best_bid_yes + best_bid_no > 1000:
    edge_bps = (best_bid_yes + best_bid_no) - 1000
    executable_size = min(best_bid_size_yes, best_bid_size_no)
    IF edge_bps > MIN_EDGE_THRESHOLD:
        LOG OPPORTUNITY → SELL_BOTH
```
Execution: Mint YES+NO tokens via CTF ($1.00) → Sell YES at bid + Sell NO at bid. Requires pre-minted token inventory (on-chain mint takes 2-6 seconds).

### Fee Consideration
Polymarket charges taker fees. Subtract fee from edge before declaring opportunity profitable.
Make `MIN_EDGE_THRESHOLD` configurable (start at 0 to log everything, then tune).

---

## Build Phases

### PHASE 1: Connection Test
**Goal:** Connect to Polymarket WebSocket, receive and print raw messages.

Create a minimal C++ program that:
1. Resolves `ws-subscriptions-clob.polymarket.com`
2. Establishes TCP connection
3. Performs TLS handshake
4. Performs WebSocket upgrade to `/ws/market`
5. Sends subscription message for ONE known active token
6. Prints raw JSON messages as they arrive
7. Handles WebSocket pings (boost::beast auto-pong)
8. Gracefully handles disconnection

**Use this token for testing (Fed chair market — usually has active orderbook):**
```
Token ID: 5031084282167950494806674428243037744881029417420880897305642929037077494331
```

Print to stdout:
```
[CONNECTED] TLS handshake complete
[SUBSCRIBED] assets_ids: ["5031084282..."]
[MSG 1] event_type=book, asset_id=5031..., bids=3, asks=4, timestamp=...
[MSG 2] event_type=last_trade_price, price=0.52, side=BUY, size=100
...
```

**Test:** Program connects, receives at least one "book" event, prints it, and stays connected for 60+ seconds without crashing.

---

### PHASE 2: JSON Parser + Orderbook
**Goal:** Parse incoming JSON into structured orderbook with zero-alloc hot path.

1. Integrate simdjson (single header: simdjson.h + simdjson.cpp)
2. Create `Orderbook` struct:
```cpp
struct PriceLevel {
    uint16_t price;    // × 1000
    uint32_t size;     // whole dollars
};

struct alignas(64) Orderbook {  // cache-line aligned
    PriceLevel bids[50];
    PriceLevel asks[50];
    uint8_t bid_count;
    uint8_t ask_count;
    uint16_t best_bid;    // quick access, no scan needed
    uint16_t best_ask;
    uint32_t best_bid_size;
    uint32_t best_ask_size;
    uint64_t timestamp;
    uint64_t local_update_ns;  // our clock when we processed this
};
```
3. Pre-allocate parser and orderbooks at startup (one per token)
4. On each "book" event: parse JSON → update orderbook in place → update best_bid/best_ask
5. On each "last_trade_price" event: log the trade

Print to stdout on each book update:
```
[BOOK] YES | bid=480(30) ask=520(25) spread=40 | 2.3μs parse
[BOOK] NO  | bid=470(15) ask=540(20) spread=70 | 1.8μs parse
```

**Test:** Parse 100 consecutive book events without any heap allocation (verify with a custom allocator or valgrind).

---

### PHASE 3: Multi-Token Subscription (YES + NO pairs)
**Goal:** Subscribe to both YES and NO tokens for a contract and maintain paired orderbooks.

1. Create a `Contract` struct:
```cpp
struct Contract {
    std::string condition_id;
    std::string asset_name;       // "BTC-UP-30min"
    std::string token_id_yes;
    std::string token_id_no;
    Orderbook book_yes;
    Orderbook book_no;
};
```
2. Subscribe to both token IDs in a single WebSocket message
3. Route incoming events to the correct orderbook by matching `asset_id`
4. Use a pre-built lookup: `std::unordered_map<std::string_view, Contract*>` mapping token_id → contract (built at startup, never modified on hot path)

Print on each update:
```
[CONTRACT] BTC-UP-30min | YES: 480/520 | NO: 470/540 | sum_asks=990 sum_bids=950
```

**Test:** Both YES and NO orderbooks update correctly. Verify by comparing against Polymarket website prices.

---

### PHASE 4: Arbitrage Detector
**Goal:** Detect and log arbitrage opportunities on every orderbook update.

1. After every orderbook update, run arb check:
```cpp
void check_arbitrage(const Contract& c) {
    // BUY-BOTH: can we buy YES + NO for less than $1.00?
    uint16_t cost = c.book_yes.best_ask + c.book_no.best_ask;
    if (cost < 1000) {
        int edge = 1000 - cost;  // in tenths of cents
        uint32_t size = std::min(c.book_yes.best_ask_size, c.book_no.best_ask_size);
        log_opportunity("BUY_BOTH", c, edge, size);
    }
    
    // SELL-BOTH: can we sell YES + NO for more than $1.00?
    uint16_t proceeds = c.book_yes.best_bid + c.book_no.best_bid;
    if (proceeds > 1000) {
        int edge = proceeds - 1000;
        uint32_t size = std::min(c.book_yes.best_bid_size, c.book_no.best_bid_size);
        log_opportunity("SELL_BOTH", c, edge, size);
    }
}
```
2. Log EVERY check, not just hits — useful for distribution analysis:
```
[ARB CHECK] BUY_BOTH | cost=990 | edge=10 (1.0%) | size=$25 | OPPORTUNITY
[ARB CHECK] BUY_BOTH | cost=1020 | edge=-20 | NO_ARB
[ARB CHECK] SELL_BOTH | proceeds=1010 | edge=10 (1.0%) | size=$15 | OPPORTUNITY
```

**Test:** Manually verify: if YES ask is 0.48 and NO ask is 0.49, the detector should fire BUY_BOTH with edge=30 (3.0%).

---

### PHASE 5: Paper Trade Logger
**Goal:** Track virtual positions, calculate theoretical P&L, log everything to CSV.

1. When an opportunity is detected and exceeds `MIN_EDGE_THRESHOLD`:
   - Record a virtual "entry" with timestamp, prices, and size
   - Track the position until the contract resolves (or a configurable hold period)
   - Calculate theoretical P&L: `profit = $1.00 * size - (ask_yes + ask_no) * size - fees`

2. CSV output (append to file, background thread flushes):
```csv
timestamp_ns,contract,arb_type,ask_yes,ask_no,bid_yes,bid_no,cost_or_proceeds,edge_bps,size_usd,theoretical_pnl,latency_ws_recv_to_detect_us
1708812345000000000,BTC-UP-30min,BUY_BOTH,480,490,470,480,970,30,25,0.75,4.2
```

3. Latency columns — stamp at every stage:
   - `t0_ws_recv_ns`: when boost::beast read returned
   - `t1_parse_done_ns`: after simdjson parse
   - `t2_book_updated_ns`: after orderbook write
   - `t3_arb_checked_ns`: after arbitrage check
   - `t4_logged_ns`: after writing to ring buffer
   - All as nanoseconds from `clock_gettime(CLOCK_MONOTONIC)`

4. Print summary every 60 seconds to stdout:
```
[SUMMARY 60s] msgs=342 | book_events=48 | trades=12 | arb_checks=96
              | opportunities=2 (BUY_BOTH=1, SELL_BOTH=1)
              | avg_edge=15bps | max_edge=32bps
              | latency p50=3.2μs p99=8.1μs p99.9=41.2μs
```

**Test:** Run for 10 minutes. CSV file should contain entries. Verify P&L calculations manually against a few logged opportunities.

---

### PHASE 6: Multi-Contract Support (BTC + ETH)
**Goal:** Monitor multiple contracts simultaneously.

1. Extend to support N contracts in a single WebSocket connection
2. Subscribe to all token IDs at once:
```json
{
    "assets_ids": ["btc_yes_token", "btc_no_token", "eth_yes_token", "eth_no_token"],
    "type": "market"
}
```
3. Route events to correct contract by asset_id lookup
4. Run arb check independently per contract on every update
5. Optionally: detect cross-contract correlations (BTC and ETH moving together)

**Configuration:** Contract definitions (token IDs, names) loaded from a JSON config file at startup, not hardcoded.

**Test:** Both BTC and ETH contracts updating independently. Arb detection running on both.

---

### PHASE 7: Reconnection & Resilience
**Goal:** Handle disconnections, errors, and long-running stability.

1. Exponential backoff reconnection: 1s → 2s → 4s → 8s → max 30s
2. On reconnect: re-subscribe to all tokens, clear stale orderbook data
3. Detect stale data: if no message received for 30 seconds, force reconnect
4. Signal handling: SIGINT/SIGTERM → graceful shutdown, flush logs, print final summary
5. Log all errors with context (not just "connection failed" — include errno, SSL error string, etc.)

**Test:** Kill the network (iptables drop), verify reconnection. Run for 24 hours without crash.

---

## Project Structure
```
polymarket-arb/
├── CMakeLists.txt
├── config.json                  # contract definitions, thresholds
├── deps/
│   ├── simdjson.h
│   └── simdjson.cpp
├── src/
│   ├── main.cpp                 # entry point, signal handling, main loop
│   ├── websocket.hpp/.cpp       # connection, subscription, read loop
│   ├── parser.hpp/.cpp          # simdjson parsing, price conversion
│   ├── orderbook.hpp/.cpp       # Orderbook struct, update logic
│   ├── arbitrage.hpp/.cpp       # detection logic
│   ├── logger.hpp/.cpp          # CSV writer, ring buffer, summary stats
│   └── types.hpp                # shared types, constants
├── tests/
│   ├── test_price_parser.cpp    # unit test price string → uint16_t
│   ├── test_orderbook.cpp       # unit test book updates
│   └── test_arbitrage.cpp       # unit test arb detection with mock data
└── logs/                        # output CSV files
```

## CMake Dependencies
```cmake
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
target_link_libraries(arb_detector
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)
```

## Build & Run
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
./arb_detector --config ../config.json
```

---

## Key Constraints & Rules

1. **ZERO heap allocation on hot path** after startup. Pre-allocate everything: orderbooks, parser, buffers, string lookup tables.
2. **No floating point on hot path.** All prices as `uint16_t × 1000`. All sizes as `uint32_t`.
3. **No `std::string` construction on hot path.** Use `std::string_view` for JSON field access.
4. **Cache-line align hot data.** `alignas(64)` on Orderbook structs.
5. **Never do file I/O on hot path.** Log to a ring buffer, background thread flushes.
6. **Timestamp everything.** Nanosecond precision at T0 through T4 on every message.
7. **Keep it simple.** No fancy frameworks. Raw boost::beast, raw simdjson, raw structs. Every abstraction layer adds latency.
8. **Config file for all parameters.** Token IDs, thresholds, intervals — nothing hardcoded except protocol constants.

---

## Config File Example (config.json)
```json
{
    "websocket_url": "wss://ws-subscriptions-clob.polymarket.com/ws/market",
    "min_edge_threshold_bps": 0,
    "taker_fee_bps": 100,
    "summary_interval_seconds": 60,
    "log_file": "logs/arb_log.csv",
    "ping_interval_seconds": 15,
    "reconnect_max_delay_seconds": 30,
    "contracts": [
        {
            "name": "BTC-UP-30min",
            "condition_id": "0x...",
            "token_id_yes": "...",
            "token_id_no": "..."
        },
        {
            "name": "ETH-UP-30min",
            "condition_id": "0x...",
            "token_id_yes": "...",
            "token_id_no": "..."
        }
    ]
}
```

---

## How to Find Active Token IDs

Query the Polymarket API for active 30-min crypto contracts:
```bash
curl -s "https://clob.polymarket.com/markets?next_cursor=MA==" | python3 -c "
import sys, json
markets = json.load(sys.stdin)
for m in markets[:10]:
    print(f\"Market: {m.get('question','?')[:80]}\")
    for t in m.get('tokens', []):
        print(f\"  {t.get('outcome','?')}: {t['token_id'][:40]}...\")
    print()
"
```

Or check the Polymarket website, find a BTC/ETH 30-min market, and extract token IDs from the page source or API.

---

## Success Criteria

After all phases complete, the system should:
- [ ] Connect to Polymarket WebSocket and stay connected 24/7
- [ ] Parse all message types without error
- [ ] Maintain accurate real-time orderbooks for YES and NO tokens
- [ ] Detect BUY_BOTH and SELL_BOTH arbitrage opportunities
- [ ] Log every opportunity with full context and latency data to CSV
- [ ] Print periodic summaries with opportunity count and latency stats
- [ ] Handle disconnections with automatic reconnection
- [ ] Process messages with < 10μs latency (T0 to T3)
- [ ] Run with zero heap allocations on the hot path
- [ ] Support multiple contracts from a config file
