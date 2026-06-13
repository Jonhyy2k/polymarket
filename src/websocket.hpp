#pragma once

#include "types.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>

#include <string>
#include <functional>
#include <atomic>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

struct WebSocketClientStats {
    uint64_t connect_attempts = 0;
    uint64_t successful_connects = 0;
    uint64_t reconnect_cycles = 0;
    uint64_t connect_failures = 0;
    uint64_t subscribe_failures = 0;
    uint64_t stale_reconnects = 0;
    uint64_t timeout_polls = 0;
    uint64_t read_errors = 0;
    uint64_t closed_events = 0;
    uint64_t ping_count = 0;
};

class WebSocketClient {
public:
    // Zero-copy callback: raw pointer + length from beast buffer, no std::string.
    // recv_time is steady_clock (pipeline timing); recv_wall_ns is CLOCK_REALTIME
    // captured at the same instant (for feed-delivery latency vs exchange ts).
    using MessageCallback =
        std::function<void(const char* data, size_t len, NanoTime recv_time, NanoTime recv_wall_ns)>;
    using StopCallback = std::function<bool()>;

    explicit WebSocketClient(const Config& config);
    ~WebSocketClient();

    // Connect and subscribe to all tokens in config
    bool connect();

    // Start reading messages (blocks until disconnect or error)
    void run(MessageCallback on_message, StopCallback should_stop = {});

    // Graceful shutdown
    void stop();

    bool is_connected() const { return connected_; }
    WebSocketClientStats stats() const { return stats_; }

private:
    const Config& config_;
    net::io_context ioc_;
    ssl::context ssl_ctx_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
    beast::flat_buffer buffer_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};
    WebSocketClientStats stats_{};

    bool do_connect();
    bool do_subscribe();
    void send_ping();
};
