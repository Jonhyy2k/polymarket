#pragma once

#include "parser.hpp"
#include "types.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

enum class MetricsEventType : uint8_t {
    MESSAGE,
    BOOK,
    PRICE_CHANGE,
    BEST_BID_ASK,
    TRADE,
    EDGE_SAMPLE
};

struct MetricsEvent {
    MetricsEventType type = MetricsEventType::MESSAGE;
    uint32_t msg_bytes = 0;
    uint16_t event_count = 1;
    bool     has_latency_sample = false;
    float    queue_us = 0.0f;
    float    parse_us = 0.0f;
    float    book_us = 0.0f;
    float    arb_us = 0.0f;
    float    e2e_us = 0.0f;
    float    feed_delivery_ms = 0.0f;   // recv_wall - newest event.timestamp (geography)
    bool     has_feed_delivery = false;
    uint16_t arb_checks = 0;
    bool     arb_checked = false;
    NanoTime sample_time_ns = 0;
    std::string_view edge_label;
    ArbKind  arb_kind = ArbKind::BUY_BOTH;
    uint16_t reference_value = 0;
    uint8_t  leg_count = 0;
    int16_t  raw_edge_bps = 0;
    int16_t  net_edge_bps = 0;
};

struct alignas(64) MessageSlot {
    size_t   len = 0;
    NanoTime recv_time = 0;       // steady_clock at receipt (pipeline timing)
    NanoTime recv_wall_ns = 0;    // CLOCK_REALTIME at receipt (feed-delivery latency)
    alignas(64) char data[MessageParser::kBufferCapacity + simdjson::SIMDJSON_PADDING];
};

inline size_t round_up_power_of_two(size_t value) noexcept {
    if (value <= 2) return 2;
    --value;
    for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1) {
        value |= value >> shift;
    }
    return value + 1;
}

template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t requested_capacity)
        : capacity_(round_up_power_of_two(requested_capacity))
        , mask_(capacity_ - 1)
        , slots_(std::make_unique<T[]>(capacity_)) {}

    template <typename Writer>
    bool try_push(Writer&& writer) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = head + 1;
        const size_t tail = tail_.load(std::memory_order_acquire);
        if (next - tail > capacity_) {
            return false;
        }

        writer(slots_[head & mask_]);
        update_high_watermark(next - tail);
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool try_push_copy(const T& value) {
        return try_push([&](T& slot) { slot = value; });
    }

    bool front(const T*& item) const {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        item = &slots_[tail & mask_];
        return true;
    }

    bool front(T*& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        item = &slots_[tail & mask_];
        return true;
    }

    void pop() {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        tail_.store(tail + 1, std::memory_order_release);
    }

    bool empty() const {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

    size_t size() const {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    size_t capacity() const {
        return capacity_;
    }

    size_t peak_size() const {
        return high_watermark_.load(std::memory_order_relaxed);
    }

private:
    void update_high_watermark(size_t size) {
        size_t current = high_watermark_.load(std::memory_order_relaxed);
        while (size > current &&
               !high_watermark_.compare_exchange_weak(
                   current, size, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }

    const size_t capacity_;
    const size_t mask_;
    std::unique_ptr<T[]> slots_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) std::atomic<size_t> high_watermark_{0};
};

inline void store_message_slot(MessageSlot& slot, const char* data, size_t len,
                               NanoTime recv_time, NanoTime recv_wall_ns) {
    slot.len = len;
    slot.recv_time = recv_time;
    slot.recv_wall_ns = recv_wall_ns;
    std::memcpy(slot.data, data, len);
    std::memset(slot.data + len, 0, simdjson::SIMDJSON_PADDING);
}

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}
