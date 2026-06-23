#pragma once

// MockLiveGateway — the middle rung between Shadow and Live.
//
// Implements IExecGateway, so the OMS / reconcile / risk / ACR / inventory code
// is completely unaware of it (mode-agnostic). On every create it builds the real
// Polymarket v2 order payload and computes the full EIP-712 digest (the 32 bytes
// an EOA would sign) — exercising the entire serialize→hash path and letting us
// count/measure it — but it STOPS before signing: no private key, no signature,
// no network. This is how we validate the live order format and propagation
// without custody or compliance clearance.

#include "oms.hpp"          // IExecGateway, ManagedOrder, OrderSide
#include "live_order.hpp"   // SignerConfig, make_payload, digest, validate_quote
#include "eip712.hpp"

#include <cstdint>
#include <fstream>
#include <string>

namespace live {

struct MockLiveStats {
    uint64_t        submits = 0;
    uint64_t        cancels = 0;
    uint64_t        digests = 0;        // EIP-712 order digests computed (== submits)
    eip712::Bytes32 last_digest{};
};

class MockLiveGateway : public IExecGateway {
public:
    MockLiveGateway(SignerConfig cfg, bool verbose);

    // Build the v2 payload + EIP-712 digest for this create (no key, no send).
    bool submit(const ManagedOrder& order) override;
    // Cancels reference an existing order id/hash, not a fresh signature; we just
    // account + optionally log (order-hash-for-cancel is a live-path follow-up).
    bool cancel(const ManagedOrder& order) override;

    const MockLiveStats& stats() const noexcept { return stats_; }

    // Startup banner: prints ORDER_TYPEHASH + both domain separators + the
    // UNVERIFIED-against-chain caveat so the constants are reconcilable by eye.
    static void describe();

private:
    SignerConfig cfg_;
    bool         verbose_;
    uint64_t     next_salt_;   // seeded from wall clock; ++ per order
    MockLiveStats stats_;
};

// Single-writer (executor/parser thread) audit of quotes a live venue would
// reject or that would earn no reward: off-grid price, a side beyond max_spread,
// or a self-cross. Writes are rare (only when ACR adjust pushes a side out), so
// a plain flushed ofstream on this one thread is fine and stays off the hot path.
class NearMissLiveLog {
public:
    explicit NearMissLiveLog(const std::string& path);
    void record(std::string_view token, uint32_t mid2, Price tick,
                const DesiredQuotes& q, const QuoteIssues& issues);
    uint64_t count() const noexcept { return count_; }
    bool open() const { return static_cast<bool>(file_); }

private:
    std::ofstream file_;
    uint64_t      count_ = 0;
};

}  // namespace live
