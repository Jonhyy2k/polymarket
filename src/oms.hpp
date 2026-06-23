#pragma once

// Order Management System (shadow mode).
//
// On Polymarket you do NOT run a matching engine — their CLOB matches and the
// Polygon CTF Exchange settles. This OMS is the client-side authority over our
// own order lifecycle: it diffs the strategy's DesiredQuotes against our live
// orders and emits create/cancel commands, enforces pre-trade risk, and tracks
// position from fills.
//
// In SHADOW mode the gateway only logs intended actions (no signing, no network,
// no keys, no money). The ManagedOrder carries the fields a v2 EIP-712 order will
// need (token, side, price, size) so the live signer/REST gateway is a drop-in.
// Going live additionally requires: API auth (L2 headers), on-chain allowances
// (collateral -> Exchange, CTF approval), pUSD/USDC collateral, and the EIP-712
// signer — NONE of which exist yet and all gated on compliance clearance.

#include "types.hpp"
#include "rewards.hpp"

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

// Execution mode. The OMS, reconcile, risk gate, ACR and inventory logic are all
// mode-AGNOSTIC — they only ever see an IExecGateway. The mode just selects which
// gateway is constructed on the send (cancel-sender) thread:
//   Shadow   — log intended actions only (no keys/serialization/network).
//   MockLive — build the v2 order + EIP-712 digest (no key, no signature, no send).
//   Live     — real signer + CLOB POST (NOT built; gated on compliance/custody).
enum class ExecMode : uint8_t { Shadow, MockLive, Live };

inline const char* exec_mode_name(ExecMode m) noexcept {
    switch (m) {
        case ExecMode::Shadow:   return "shadow";
        case ExecMode::MockLive: return "mocklive";
        case ExecMode::Live:     return "live";
    }
    return "?";
}

enum class OrderSide : uint8_t { BUY, SELL };

enum class OrderState : uint8_t {
    Intended,        // created locally, not yet sent
    Submitted,       // sent, awaiting ack
    Live,            // resting on the book
    PartiallyFilled,
    Filled,
    Canceled,
    Rejected,
};

inline const char* order_side_name(OrderSide s) noexcept {
    return s == OrderSide::BUY ? "BUY" : "SELL";
}
inline const char* order_state_name(OrderState s) noexcept {
    switch (s) {
        case OrderState::Intended:        return "Intended";
        case OrderState::Submitted:       return "Submitted";
        case OrderState::Live:            return "Live";
        case OrderState::PartiallyFilled: return "PartiallyFilled";
        case OrderState::Filled:          return "Filled";
        case OrderState::Canceled:        return "Canceled";
        case OrderState::Rejected:        return "Rejected";
    }
    return "?";
}

struct ManagedOrder {
    uint64_t    client_id = 0;       // our id; maps to exchange order id once acked
    std::string token_id;            // ERC-1155 position token (v2 EIP-712 tokenId)
    OrderSide   side = OrderSide::BUY;
    Price       price = 0;           // thousandths
    Size        size = 0;            // shares
    Size        filled = 0;
    OrderState  state = OrderState::Intended;
    NanoTime    created_ns = 0;
    uint32_t    ref_mid2 = 0;         // mid×2 when placed (ACR drift detection)
    bool        neg_risk = false;     // market is neg-risk -> selects the v2 EIP-712 domain
    // exchange_order_id, salt, nonce, signature filled in by the live gateway.
    std::string exchange_order_id;

    double notional_usd() const noexcept {
        return static_cast<double>(price) / static_cast<double>(PRICE_ONE) *
               static_cast<double>(size);
    }
    Size remaining() const noexcept { return size > filled ? size - filled : 0; }
};

// Pre-trade risk limits. Checked before any create is emitted; a breach drops the
// create (and is counted), never throws. kill_switch halts all new orders.
struct RiskLimits {
    size_t max_open_orders_per_token = 2;     // one bid + one ask
    size_t max_open_orders_total     = 64;
    double max_gross_notional_usd    = 1000.0;
    double max_position_shares       = 5000.0; // |net position| per token
    // Simulated pUSD collateral allowance: total gross notional of resting orders
    // may not exceed the wallet's approved/available pUSD. 0 => unlimited (off).
    // First-order proxy of the on-chain constraint (does not yet model fills
    // consuming collateral, nor the YES/NO USDC-both-sides netting). See README.
    double max_collateral_usd        = 0.0;
    bool   kill_switch               = false;
};

// Abstract execution gateway. ShadowGateway logs; a future LiveGateway signs +
// POSTs to the CLOB. The OMS only ever talks to this interface.
class IExecGateway {
public:
    virtual ~IExecGateway() = default;
    // Returns true if the action was accepted by the gateway (in shadow: always,
    // and it immediately drives the order to Live via the ack callback path).
    virtual bool submit(const ManagedOrder& order) = 0;
    virtual bool cancel(const ManagedOrder& order) = 0;
};

// Shadow gateway: accepts every action, drives orders Live synchronously, and
// optionally logs the intended create/cancel. NO signing, NO network, NO keys.
// Swapping this for a LiveGateway (EIP-712 sign + CLOB POST) is the only change
// needed on the send path once compliance + auth + allowances are in place.
class ShadowGateway : public IExecGateway {
public:
    explicit ShadowGateway(bool verbose = false) : verbose_(verbose) {}
    bool submit(const ManagedOrder& order) override;
    bool cancel(const ManagedOrder& order) override;
    uint64_t submitted() const noexcept { return submitted_; }
    uint64_t canceled() const noexcept { return canceled_; }
private:
    bool verbose_;
    uint64_t submitted_ = 0;
    uint64_t canceled_ = 0;
};

// A command handed from the OMS (parser thread) to the cancel-sender thread via
// an SPSC ring, so the hot path never blocks on the gateway's I/O. decided_ns is
// stamped at enqueue to measure decision->execute (in-process send) latency.
struct OrderCommand {
    enum class Kind : uint8_t { CREATE, CANCEL };
    Kind         kind = Kind::CREATE;
    ManagedOrder order;
    NanoTime     decided_ns = 0;
};

// Gateway used BY the OMS that just enqueues commands (via a sink) for the sender
// thread to execute. Keeps order-state mutation single-threaded (parser thread)
// while the actual send (ShadowGateway now, LiveGateway later) runs off-path.
class ThreadedGateway : public IExecGateway {
public:
    using Sink = std::function<bool(const OrderCommand&)>;
    explicit ThreadedGateway(Sink sink) : sink_(std::move(sink)) {}
    bool submit(const ManagedOrder& o) override {
        return sink_(OrderCommand{OrderCommand::Kind::CREATE, o, now_ns()});
    }
    bool cancel(const ManagedOrder& o) override {
        return sink_(OrderCommand{OrderCommand::Kind::CANCEL, o, now_ns()});
    }
private:
    Sink sink_;
};

// Read-only snapshot of our resting orders on one token, for the ACR engine.
struct LiveQuoteRef {
    bool     has = false;
    Price    price = 0;
    uint32_t ref_mid2 = 0;   // mid×2 when placed
    Size     size = 0;
    uint64_t client_id = 0;
};
struct LiveSide { LiveQuoteRef bid, ask; };

struct OmsStats {
    uint64_t creates = 0;
    uint64_t cancels = 0;
    uint64_t replaces = 0;
    uint64_t risk_rejects = 0;
    uint64_t fills = 0;
    double   filled_notional_usd = 0.0;
};

class Oms {
public:
    Oms(IExecGateway& gateway, RiskLimits limits)
        : gateway_(gateway), limits_(limits) {}

    // Diff desired two-sided quote vs current live orders for one token; emit the
    // minimal set of cancel/create actions (cancel-then-create on any price/size
    // change). Idempotent: identical desired vs live => no action. mid2 (mid×2 of
    // the token's book) is stamped on placed orders for ACR drift detection.
    void reconcile(const std::string& token_id, const DesiredQuotes& desired,
                   uint32_t mid2 = 0, bool neg_risk = false);

    // Snapshot of our resting orders for a token (for the ACR engine).
    LiveSide live_side(const std::string& token_id) const;

    // Cancel every live order for a token (used on shutdown / resync / kill).
    void cancel_all(const std::string& token_id);
    void cancel_everything();

    // Apply a fill (from the shadow fill simulator or, live, the user WS).
    void apply_fill(const std::string& token_id, uint64_t client_id, Size fill_size);

    const OmsStats& stats() const noexcept { return stats_; }
    double net_position(const std::string& token_id) const;
    size_t open_order_count() const;

private:
    struct TokenBook {
        ManagedOrder bid;   // at most one resting order per side in this strategy
        ManagedOrder ask;
        bool has_bid = false;
        bool has_ask = false;
        double net_position = 0.0;  // +long shares (BUY filled) - short (SELL filled)
    };

    bool passes_risk(const std::string& token_id, const RewardQuote& q, OrderSide side);
    void place(TokenBook& tb, const std::string& token_id, const RewardQuote& q,
               OrderSide side, uint32_t mid2, bool neg_risk);
    void drop(ManagedOrder& order, bool& has_flag);

    IExecGateway& gateway_;
    RiskLimits    limits_;
    OmsStats      stats_{};
    uint64_t      next_client_id_ = 1;
    std::vector<std::pair<std::string, TokenBook>> books_;  // small N (markets)

    TokenBook& book_for(const std::string& token_id);
    const TokenBook* find_book(const std::string& token_id) const;
};
