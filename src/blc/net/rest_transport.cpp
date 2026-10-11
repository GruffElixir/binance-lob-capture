#include "blc/net/rest_transport.hpp"

#include "blc/core/time.hpp"
#include "blc/version.hpp"

#include <charconv>
#include <optional>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

namespace blc {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

namespace {

int header_int(const http::fields& fields, beast::string_view name) {
    const auto it = fields.find(name);
    if (it == fields.end()) {
        return 0;
    }
    const auto v = it->value();
    int out = 0;
    const auto r = std::from_chars(v.data(), v.data() + v.size(), out);
    return r.ec == std::errc() ? out : 0;
}

}  // namespace

class BeastRestTransport::Session : public std::enable_shared_from_this<Session> {
public:
    Session(asio::io_context& io, asio::ssl::context& tls, JsonWorkspace& workspace,
            const NetTuning& tuning, Endpoint endpoint, std::string target,
            std::function<void(HttpResult)> done)
        : resolver_(io),
          stream_(io, tls),
          workspace_(workspace),
          tuning_(tuning),
          endpoint_(std::move(endpoint)),
          target_(std::move(target)),
          done_(std::move(done)) {}

    void start() {
        deadline_ = std::chrono::steady_clock::now() + tuning_.rest_timeout;
        // SNI (the macro form uses old-style casts) and hostname verification.
        SSL_ctrl(stream_.native_handle(), SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
                 const_cast<char*>(endpoint_.host.c_str()));
        stream_.set_verify_callback(asio::ssl::host_name_verification(endpoint_.host));
        resolver_.async_resolve(
            endpoint_.host, endpoint_.port,
            [self = shared_from_this()](const beast::error_code& ec,
                                        const tcp::resolver::results_type& results) {
                self->on_resolve(ec, results);
            });
    }

    void cancel() noexcept {
        stopped_ = true;
        resolver_.cancel();
        beast::error_code ignored;
        beast::get_lowest_layer(stream_).socket().close(ignored);
    }

private:
    void on_resolve(const beast::error_code& ec, const tcp::resolver::results_type& results) {
        if (ec || stopped_) {
            return fail();
        }
        beast::get_lowest_layer(stream_).expires_at(deadline_);
        beast::get_lowest_layer(stream_).async_connect(
            results, [self = shared_from_this()](const beast::error_code& e, const tcp::endpoint&) {
                self->on_connect(e);
            });
    }

    void on_connect(const beast::error_code& ec) {
        if (ec || stopped_) {
            return fail();
        }
        beast::get_lowest_layer(stream_).expires_at(deadline_);
        stream_.async_handshake(
            asio::ssl::stream_base::client,
            [self = shared_from_this()](const beast::error_code& e) { self->on_handshake(e); });
    }

    void on_handshake(const beast::error_code& ec) {
        if (ec || stopped_) {
            return fail();
        }
        request_.version(11);
        request_.method(http::verb::get);
        request_.target(target_);
        request_.set(http::field::host, endpoint_.host);
        request_.set(http::field::user_agent, "binance_capture/" + std::string(kVersion));
        request_.set(http::field::connection, "close");
        beast::get_lowest_layer(stream_).expires_at(deadline_);
        http::async_write(stream_, request_,
                          [self = shared_from_this()](const beast::error_code& e, std::size_t) {
                              self->on_write(e);
                          });
    }

    void on_write(const beast::error_code& ec) {
        if (ec || stopped_) {
            return fail();
        }
        parser_.emplace();
        parser_->body_limit(kMaxNetworkMessageBytes);
        beast::get_lowest_layer(stream_).expires_at(deadline_);
        http::async_read(stream_, buffer_, *parser_,
                         [self = shared_from_this()](const beast::error_code& e, std::size_t) {
                             self->on_read(e);
                         });
    }

    void on_read(const beast::error_code& ec) {
        if (ec || stopped_) {
            return fail();
        }
        HttpResult r;
        r.recv_ns = wall_clock_ns();
        r.steady_ns = steady_clock_ns();
        auto& response = parser_->get();
        r.status = static_cast<int>(response.result_int());
        r.retry_after_s = header_int(response.base(), "Retry-After");
        r.used_weight_1m = header_int(response.base(), "X-MBX-USED-WEIGHT-1M");
        r.body = std::move(response.body());
        if (r.status == 200 && !workspace_.object(r.body)) {
            r.transport_error = true;  // invalid JSON: retryable, never recorded
            r.body.clear();
        } else if (r.status == 400) {
            std::optional<std::int64_t> code;
            if (workspace_.object(r.body, &code)) {
                r.binance_error_code = code;
            }
        }
        close();
        finish(std::move(r));
    }

    void fail() {
        close();
        HttpResult r;
        r.transport_error = true;
        finish(std::move(r));
    }

    void close() noexcept {
        beast::error_code ignored;
        beast::get_lowest_layer(stream_).socket().close(ignored);
    }

    void finish(HttpResult r) {
        if (!done_) {
            return;
        }
        auto done = std::move(done_);
        done_ = nullptr;
        done(std::move(r));
    }

    tcp::resolver resolver_;
    beast::ssl_stream<beast::tcp_stream> stream_;
    JsonWorkspace& workspace_;
    NetTuning tuning_;
    Endpoint endpoint_;
    std::string target_;
    std::function<void(HttpResult)> done_;
    std::chrono::steady_clock::time_point deadline_;
    http::request<http::empty_body> request_;
    beast::flat_buffer buffer_;
    std::optional<http::response_parser<http::string_body>> parser_;
    bool stopped_ = false;
};

BeastRestTransport::BeastRestTransport(asio::io_context& io, asio::ssl::context& tls,
                                       JsonWorkspace& workspace, const NetTuning& tuning)
    : io_(io), tls_(tls), workspace_(workspace), tuning_(tuning) {}

BeastRestTransport::~BeastRestTransport() {
    cancel_all();
}

void BeastRestTransport::get(const Endpoint& endpoint, std::string target,
                             std::function<void(HttpResult)> done) {
    if (stopped_) {
        return;
    }
    // Forget sessions that have finished.
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                   [](const std::weak_ptr<Session>& s) { return s.expired(); }),
                    sessions_.end());
    auto session = std::make_shared<Session>(io_, tls_, workspace_, tuning_, endpoint,
                                             std::move(target), std::move(done));
    sessions_.push_back(session);
    session->start();
}

void BeastRestTransport::cancel_all() noexcept {
    stopped_ = true;
    for (auto& weak : sessions_) {
        if (auto s = weak.lock()) {
            s->cancel();
        }
    }
    sessions_.clear();
}

}  // namespace blc
