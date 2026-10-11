#include "blc/net/ws_shard.hpp"

#include "blc/core/time.hpp"
#include "blc/parse/json_workspace.hpp"
#include "blc/version.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
namespace blc {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
class WsShard::Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(const std::shared_ptr<WsShard>& owner)
        : owner_(owner),
          resolver_(owner->io_),
          stream_(owner->io_, owner->tls_),
          close_timer_(owner->io_),
          buffer_(kMaxNetworkMessageBytes) {}
    void start() {
        const auto owner = active();
        if (!owner || owner->stopping_)
            return;
        owner->state_ = State::Resolving;
        resolver_.async_resolve(
            owner->endpoint_.host, owner->endpoint_.port,
            [self = shared_from_this()](beast::error_code ec,
                                        const tcp::resolver::results_type& results) {
                self->resolved(ec, results);
            });
    }
    void abort() noexcept {
        resolver_.cancel();
        close_timer_.cancel();
        // Boost 1.74 retains its handshake/idle timer after a raw socket cancellation.
        // Disable both timers so an aborted handshake cannot keep io.run() alive.
        websocket::stream_base::timeout timeout;
        timeout.handshake_timeout = websocket::stream_base::none();
        timeout.idle_timeout = websocket::stream_base::none();
        timeout.keep_alive_pings = false;
        stream_.set_option(timeout);
        beast::error_code ignored;
        beast::get_lowest_layer(stream_).socket().close(ignored);
    }
    void shutdown(bool graceful) {
        resolver_.cancel();
        if (!graceful) {
            abort();
            return;
        }
        const auto owner = active();
        if (!owner)
            return;
        close_timer_.expires_after(owner->tuning_.close_timeout);
        close_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (!ec)
                self->abort();
        });
        stream_.async_close(websocket::close_code::normal,
                            [self = shared_from_this()](beast::error_code) {
                                self->abort();
                                if (const auto shard = self->active())
                                    shard->state_ = State::Closed;
                            });
    }

private:
    std::shared_ptr<WsShard> active() const {
        auto owner = owner_.lock();
        return owner && owner->session_.get() == this ? owner : nullptr;
    }
    bool failed(const beast::error_code& ec) {
        const auto owner = active();
        if (!owner || owner->stopping_)
            return true;
        if (!ec)
            return false;
        owner->retry(false, ec.message());
        return true;
    }
    void resolved(beast::error_code ec, const tcp::resolver::results_type& results) {
        if (failed(ec))
            return;
        const auto owner = active();
        owner->state_ = State::Connecting;
        beast::get_lowest_layer(stream_).expires_after(owner->tuning_.ws_handshake_timeout);
        beast::get_lowest_layer(stream_).async_connect(
            results, [self = shared_from_this()](beast::error_code e, const tcp::endpoint&) {
                self->connected(e);
            });
    }
    void connected(beast::error_code ec) {
        if (failed(ec))
            return;
        const auto owner = active();
        owner->state_ = State::TlsHandshake;
        if (SSL_ctrl(stream_.next_layer().native_handle(), SSL_CTRL_SET_TLSEXT_HOSTNAME,
                     TLSEXT_NAMETYPE_host_name,
                     const_cast<char*>(owner->endpoint_.host.c_str())) != 1) {
            owner->retry(false, "cannot set TLS server name");
            return;
        }
        stream_.next_layer().set_verify_callback(
            asio::ssl::host_name_verification(owner->endpoint_.host));
        beast::get_lowest_layer(stream_).expires_after(owner->tuning_.ws_handshake_timeout);
        stream_.next_layer().async_handshake(
            asio::ssl::stream_base::client,
            [self = shared_from_this()](beast::error_code e) { self->tls_handshake(e); });
    }
    void tls_handshake(beast::error_code ec) {
        if (failed(ec))
            return;
        const auto owner = active();
        owner->state_ = State::WsHandshake;
        beast::get_lowest_layer(stream_).expires_never();
        websocket::stream_base::timeout timeout;
        timeout.handshake_timeout = owner->tuning_.ws_handshake_timeout;
        timeout.idle_timeout = owner->tuning_.ws_idle_timeout;
        timeout.keep_alive_pings = true;
        stream_.set_option(timeout);
        stream_.read_message_max(kMaxNetworkMessageBytes);
        stream_.set_option(websocket::stream_base::decorator([](websocket::request_type& request) {
            request.set(beast::http::field::user_agent, "binance_capture/" + std::string(kVersion));
        }));
        stream_.async_handshake(
            owner->endpoint_.host + ":" + owner->endpoint_.port, owner->target_,
            [self = shared_from_this()](beast::error_code e) { self->opened(e); });
    }
    void opened(beast::error_code ec) {
        if (failed(ec))
            return;
        const auto owner = active();
        if (owner->connected_once_) {
            ++owner->epoch_;
            IoMetrics::add(owner->metrics_.reconnects);
        }
        owner->connected_once_ = true;
        owner->epoch_open_ = true;
        owner->opened_at_ = std::chrono::steady_clock::now();
        owner->router_.on_connected(owner->epoch_);
        IoMetrics::add(owner->metrics_.connects);
        owner->state_ = State::Open;
        owner->arm_fault();
        read();
    }
    void read() {
        stream_.async_read(buffer_, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            self->received(ec);
        });
    }
    void received(beast::error_code ec) {
        const auto owner = active();
        if (!owner)
            return;
        if (ec) {
            owner->router_.on_disconnected();
            if (!owner->stopping_)
                owner->retry(false, ec.message());
            return;
        }
        const auto recv = wall_clock_ns();
        const auto steady = steady_clock_ns();
        owner->observe_capacity(buffer_.capacity(), false);
        const auto bytes = buffer_.data();
        const auto result = owner->router_.on_frame(
            {static_cast<const char*>(bytes.data()), bytes.size()}, recv, steady);
        buffer_.consume(buffer_.size());
        if (buffer_.capacity() > 256 * 1024)
            buffer_.shrink_to_fit();
        owner->observe_capacity(buffer_.capacity(), true);
        if (owner->stopping_)
            return;
        if (result == FrameResult::LostNow) {
            owner->retry(true, "frame loss");
            return;
        }
        read();
    }
    std::weak_ptr<WsShard> owner_;
    tcp::resolver resolver_;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> stream_;
    asio::steady_timer close_timer_;
    beast::flat_buffer buffer_;
};
WsShard::WsShard(asio::io_context& io, asio::ssl::context& tls, Endpoint endpoint,
                 std::string target, FrameRouter& router, AttemptLimiter& limiter,
                 const NetTuning& tuning, IoMetrics& metrics, std::chrono::seconds fault_every)
    : io_(io),
      tls_(tls),
      endpoint_(std::move(endpoint)),
      target_(std::move(target)),
      router_(router),
      limiter_(limiter),
      tuning_(tuning),
      metrics_(metrics),
      backoff_(tuning.backoff_initial, tuning.backoff_cap, tuning.backoff_jitter,
               std::random_device{}()),
      timer_(io),
      fault_timer_(io),
      fault_every_(fault_every) {}
WsShard::~WsShard() {
    if (session_)
        session_->abort();
}
void WsShard::start() {
    if (state_ == State::Closed && !stopping_)
        attempt();
}
void WsShard::attempt() {
    if (stopping_)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (!limiter_.try_take(now)) {
        state_ = State::Waiting;
        IoMetrics::add(metrics_.connect_throttled);
        timer_.expires_at(limiter_.next_available(now));
        timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
            if (!ec && !self->stopping_)
                self->attempt();
        });
        return;
    }
    session_ = std::make_shared<Session>(shared_from_this());
    session_->start();
}
void WsShard::close_epoch() {
    if (!epoch_open_)
        return;
    epoch_open_ = false;
    const auto stats = router_.close_epoch();
    log(LogLevel::Info, "epoch_closed shard=" + std::to_string(router_.shard_id()) + " epoch=" +
                            std::to_string(epoch_) + " frames=" + std::to_string(stats.frames) +
                            " malformed=" + std::to_string(stats.malformed) +
                            " drops=" + std::to_string(stats.drops));
}
void WsShard::observe_capacity(std::size_t capacity, bool after_consume) {
    const auto total = metrics_.receive_capacity_bytes.load(std::memory_order_relaxed);
    metrics_.receive_capacity_bytes.store(total - reported_capacity_ + capacity,
                                          std::memory_order_relaxed);
    reported_capacity_ = capacity;
    if (observer_)
        observer_(capacity, after_consume);
}
void WsShard::retry(bool immediately, std::string_view reason) {
    fault_timer_.cancel();
    router_.on_disconnected();
    close_epoch();
    if (state_ != State::Open)
        IoMetrics::add(metrics_.connect_failures);
    if (errors_.should_log())
        log(LogLevel::Warn, "WebSocket " + endpoint_.host + ": " + std::string(reason));
    if (state_ == State::Open &&
        std::chrono::steady_clock::now() - opened_at_ >= tuning_.backoff_reset_after)
        backoff_.reset();
    if (session_)
        session_->abort();
    session_.reset();
    observe_capacity(0, true);
    if (stopping_) {
        state_ = State::Closed;
        return;
    }
    state_ = State::Waiting;
    timer_.expires_after(immediately ? std::chrono::milliseconds(0) : backoff_.next());
    timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
        if (!ec && !self->stopping_)
            self->attempt();
    });
}
void WsShard::shutdown() {
    if (stopping_)
        return;
    stopping_ = true;
    fault_timer_.cancel();
    timer_.cancel();
    router_.on_disconnected();
    const bool graceful = state_ == State::Open;
    state_ = graceful ? State::Closing : State::Closed;
    if (session_)
        session_->shutdown(graceful);
}
void WsShard::finalize() {
    stopping_ = true;
    fault_timer_.cancel();
    router_.on_disconnected();
    close_epoch();
    if (session_)
        session_->abort();
    session_.reset();
    observe_capacity(0, true);
    state_ = State::Closed;
}
void WsShard::arm_fault() {
    if (fault_every_.count() <= 0)
        return;
    fault_timer_.expires_after(fault_every_);
    fault_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
        if (!ec && !self->stopping_ && self->state_ == State::Open)
            self->retry(true, "fault-injection reconnect");
    });
}
}  // namespace blc
