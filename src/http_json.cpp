#include "http_json.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include <openssl/ssl.h>

#include <chrono>
#include <stdexcept>
#include <utility>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace {

double elapsed_ms(std::chrono::steady_clock::time_point start,
                  std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

struct HttpsSession::Impl {
    explicit Impl(std::string host_in, std::string port_in)
        : host(std::move(host_in))
        , port(std::move(port_in))
        , ssl_ctx(ssl::context::tls_client)
        , resolver(ioc) {
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(ssl::verify_peer);
    }

    using Stream = beast::ssl_stream<beast::tcp_stream>;

    bool connect(std::string& error) {
        try {
            const auto t0 = std::chrono::steady_clock::now();
            ioc.restart();
            stream = std::make_unique<Stream>(ioc, ssl_ctx);

            if (!SSL_set_tlsext_host_name(stream->native_handle(), host.c_str())) {
                error = "Failed to set TLS SNI host";
                return false;
            }

            const auto endpoints = resolver.resolve(host, port);
            beast::get_lowest_layer(*stream).connect(endpoints);
            beast::get_lowest_layer(*stream).socket().set_option(tcp::no_delay(true));
            stream->handshake(ssl::stream_base::client);

            const auto t1 = std::chrono::steady_clock::now();
            connected = true;
            ++metrics.connect_count;
            metrics.connect_ms_total += elapsed_ms(t0, t1);
            return true;
        } catch (const std::exception& ex) {
            error = ex.what();
            connected = false;
            stream.reset();
            return false;
        }
    }

    void close_silently() noexcept {
        connected = false;
        if (!stream) return;

        beast::error_code ec;
        stream->shutdown(ec);
        if (ec == net::error::eof || ec == ssl::error::stream_truncated) {
            ec = {};
        }
        stream.reset();
    }

    bool get(std::string_view target, std::string& body, std::string& error, double* request_ms) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!connected && !connect(error)) {
                return false;
            }

            try {
                const auto t0 = std::chrono::steady_clock::now();
                beast::get_lowest_layer(*stream).expires_after(std::chrono::seconds(15));

                http::request<http::empty_body> req{http::verb::get, std::string(target), 11};
                req.set(http::field::host, host);
                req.set(http::field::user_agent, "polymarket-https-session/0.1");
                req.set(http::field::accept, "application/json");
                req.keep_alive(true);

                http::write(*stream, req);

                beast::flat_buffer buffer;
                http::response<http::string_body> res;
                http::read(*stream, buffer, res);

                const auto t1 = std::chrono::steady_clock::now();
                const double elapsed = elapsed_ms(t0, t1);

                ++metrics.request_count;
                metrics.request_ms_total += elapsed;
                if (elapsed > metrics.max_request_ms) {
                    metrics.max_request_ms = elapsed;
                }

                if (res.result() != http::status::ok) {
                    error = "HTTP " + std::to_string(res.result_int()) + " from " + host;
                    if (!res.keep_alive()) {
                        close_silently();
                    }
                    return false;
                }

                body = std::move(res.body());
                metrics.bytes_received += body.size();
                if (request_ms) {
                    *request_ms = elapsed;
                }

                if (!res.keep_alive()) {
                    close_silently();
                }
                return true;
            } catch (const std::exception& ex) {
                error = ex.what();
                close_silently();
                ++metrics.retry_count;
                if (attempt == 0) {
                    continue;
                }
                return false;
            }
        }

        error = "Unreachable";
        return false;
    }

    // Generic verb request with a body + caller-supplied headers. Mirrors get()'s
    // warm-socket reuse and one-retry-on-broken-pipe, but returns the HTTP status
    // (non-2xx is not an error) so the caller can read a CLOB JSON error body.
    bool request(std::string_view method, std::string_view target,
                 std::string_view body, const HttpHeaders& extra_headers,
                 HttpResponse& resp, std::string& error,
                 std::string_view content_type, double* request_ms) {
        const http::verb verb = http::string_to_verb(std::string(method));
        if (verb == http::verb::unknown) {
            error = "Unknown HTTP method: " + std::string(method);
            return false;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!connected && !connect(error)) {
                return false;
            }
            try {
                const auto t0 = std::chrono::steady_clock::now();
                beast::get_lowest_layer(*stream).expires_after(std::chrono::seconds(15));

                http::request<http::string_body> req{verb, std::string(target), 11};
                req.set(http::field::host, host);
                req.set(http::field::user_agent, "polymarket-https-session/0.1");
                req.set(http::field::accept, "application/json");
                for (const auto& h : extra_headers) {
                    req.set(h.first, h.second);
                }
                if (!body.empty()) {
                    req.set(http::field::content_type, std::string(content_type));
                    req.body().assign(body.begin(), body.end());
                }
                req.keep_alive(true);
                req.prepare_payload();   // sets Content-Length (and zero-len for empty)

                http::write(*stream, req);

                beast::flat_buffer buffer;
                http::response<http::string_body> res;
                http::read(*stream, buffer, res);

                const auto t1 = std::chrono::steady_clock::now();
                const double elapsed = elapsed_ms(t0, t1);
                ++metrics.request_count;
                metrics.request_ms_total += elapsed;
                if (elapsed > metrics.max_request_ms) metrics.max_request_ms = elapsed;
                if (request_ms) *request_ms = elapsed;

                resp.status = res.result_int();
                resp.body   = std::move(res.body());
                metrics.bytes_received += resp.body.size();

                if (!res.keep_alive()) close_silently();
                return true;
            } catch (const std::exception& ex) {
                error = ex.what();
                close_silently();
                ++metrics.retry_count;
                if (attempt == 0) continue;   // broken keep-alive: reconnect once
                return false;
            }
        }
        error = "Unreachable";
        return false;
    }

    std::string host;
    std::string port;
    net::io_context ioc;
    ssl::context ssl_ctx;
    tcp::resolver resolver;
    std::unique_ptr<Stream> stream;
    bool connected = false;
    HttpsSessionMetrics metrics;
};

HttpsSession::HttpsSession(std::string host, std::string port)
    : impl_(std::make_unique<Impl>(std::move(host), std::move(port))) {}

HttpsSession::~HttpsSession() = default;

bool HttpsSession::get(std::string_view target, std::string& body, std::string& error,
                       double* request_ms) {
    return impl_->get(target, body, error, request_ms);
}

bool HttpsSession::request(std::string_view method, std::string_view target,
                           std::string_view body, const HttpHeaders& extra_headers,
                           HttpResponse& resp, std::string& error,
                           std::string_view content_type, double* request_ms) {
    return impl_->request(method, target, body, extra_headers, resp, error,
                          content_type, request_ms);
}

const HttpsSessionMetrics& HttpsSession::metrics() const noexcept {
    return impl_->metrics;
}
