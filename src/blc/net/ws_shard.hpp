#pragma once
#include "blc/net/attempt_limiter.hpp"
#include "blc/net/endpoints.hpp"
#include "blc/pipeline/frame_router.hpp"
#include "blc/util/logging.hpp"

#include <functional>
#include <memory>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
namespace blc {
class WsShard : public std::enable_shared_from_this<WsShard> {
public:
    enum class State {
        Waiting,
        Resolving,
        Connecting,
        TlsHandshake,
        WsHandshake,
        Open,
        Closing,
        Closed
    };
    WsShard(boost::asio::io_context&, boost::asio::ssl::context&, Endpoint, std::string target,
            FrameRouter&, AttemptLimiter&, const NetTuning&, IoMetrics&,
            std::chrono::seconds fault_every = std::chrono::seconds{0});
    ~WsShard();
    WsShard(const WsShard&) = delete;
    WsShard& operator=(const WsShard&) = delete;
    void start();
    void shutdown();
    void finalize();  // after io.run(): close epoch accounting and release the transport
    State state() const noexcept { return state_; }
    std::uint32_t epoch() const noexcept { return epoch_; }
    void set_buffer_observer(std::function<void(std::size_t, bool)> observer) {
        observer_ = std::move(observer);
    }

private:
    class Session;
    void attempt();
    void retry(bool immediately, std::string_view reason);
    void close_epoch();
    void arm_fault();
    void observe_capacity(std::size_t capacity, bool after_consume);
    boost::asio::io_context& io_;
    boost::asio::ssl::context& tls_;
    Endpoint endpoint_;
    std::string target_;
    FrameRouter& router_;
    AttemptLimiter& limiter_;
    NetTuning tuning_;
    IoMetrics& metrics_;
    Backoff backoff_;
    boost::asio::steady_timer timer_;
    boost::asio::steady_timer fault_timer_;
    std::chrono::seconds fault_every_;
    std::shared_ptr<Session> session_;
    std::function<void(std::size_t, bool)> observer_;
    std::chrono::steady_clock::time_point opened_at_;
    State state_ = State::Closed;
    std::uint32_t epoch_ = 0;
    std::size_t reported_capacity_ = 0;
    bool connected_once_ = false;
    bool epoch_open_ = false;
    bool stopping_ = false;
    RateLimitedLog errors_;
};
}  // namespace blc
