#pragma once

#include "blc/core/symbols.hpp"
#include "blc/core/types.hpp"
#include "blc/net/dispatch_policy.hpp"
#include "blc/net/endpoints.hpp"
#include "blc/pipeline/metrics.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

namespace blc {

// One HTTPS GET at a time per call; the callback runs once, on the io thread.
class RestTransport {
public:
    virtual ~RestTransport() = default;
    virtual void get(const Endpoint& endpoint, std::string target,
                     std::function<void(HttpResult)> done) = 0;
    // Cancels everything in flight (shutdown); pending callbacks still run at most once.
    virtual void cancel_all() noexcept = 0;
};

// Asio glue around DispatchPolicy (design §6.3): one steady timer armed for the policy's next
// wakeup or the next ring-full retry, the transport, and the delivery/fatal callbacks.
class RestScheduler {
public:
    using DeliveryFn =
        std::function<DeliveryResult(std::uint16_t symbol, std::uint32_t epoch, const HttpResult&)>;
    using FatalFn = std::function<void(std::string_view)>;

    RestScheduler(boost::asio::io_context& io, Venue venue, VenueEndpoints endpoints,
                  const NetTuning& tuning, RestTransport& transport, IoMetrics& metrics);
    ~RestScheduler();
    RestScheduler(const RestScheduler&) = delete;
    RestScheduler& operator=(const RestScheduler&) = delete;
    RestScheduler(RestScheduler&&) = delete;
    RestScheduler& operator=(RestScheduler&&) = delete;

    // Checks every symbol with a ticker request; `done` runs once: Ok when all are valid,
    // otherwise the first failure.
    void validate_symbols(const std::vector<SymbolSpec>& symbols,
                          std::function<void(ValidationResult)> done);

    // A snapshot request from the processor's request slot ((epoch << 32) | number).
    void want(std::uint16_t symbol, std::uint64_t slot_value);

    void set_delivery(DeliveryFn delivery) { delivery_ = std::move(delivery); }
    void set_fatal(FatalFn fatal) { fatal_ = std::move(fatal); }

    // Idempotent: stops dispatching, cancels the timer and the transport, discards waiting
    // bodies. Late completions are ignored.
    void stop();

private:
    struct Waiting {
        std::uint16_t symbol{};
        std::uint32_t epoch{};
        HttpResult result;
        DispatchPolicy::Clock::time_point retry_at;
    };

    void pump();
    void arm_timer();
    void on_timer();
    void complete(const Dispatch& d, HttpResult result);
    void complete_validation(const Dispatch& d, Outcome outcome, const HttpResult& result);
    void deliver(std::uint16_t symbol, std::uint32_t epoch, HttpResult result);
    void report_validation(ValidationResult result);

    boost::asio::io_context& io_;
    Venue venue_;
    VenueEndpoints endpoints_;
    NetTuning tuning_;
    RestTransport& transport_;
    IoMetrics& metrics_;
    DispatchPolicy policy_;
    boost::asio::steady_timer timer_;
    std::shared_ptr<char> life_ = std::make_shared<char>(0);
    std::vector<SymbolSpec> symbols_;
    std::function<void(ValidationResult)> validation_done_;
    std::size_t validations_pending_ = 0;
    std::vector<Waiting> waiting_;
    DeliveryFn delivery_;
    FatalFn fatal_;
    bool fatal_reported_ = false;
    bool stopping_ = false;
};

}  // namespace blc
