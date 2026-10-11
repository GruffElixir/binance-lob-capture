#pragma once

#include "blc/core/types.hpp"
#include "blc/net/endpoints.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace blc {

// One completed HTTP exchange as the scheduler sees it.
struct HttpResult {
    int status = 0;
    std::string body;
    int retry_after_s = 0;                           // Retry-After header; 0 when absent
    int used_weight_1m = 0;                          // X-MBX-USED-WEIGHT-1M; 0 when absent
    std::int64_t recv_ns = 0;                        // wall clock when the response was complete
    std::int64_t steady_ns = 0;                      // steady clock at the same moment
    std::optional<std::int64_t> binance_error_code;  // root "code" of a 400 body
    bool transport_error = false;  // connect/TLS/read failure, timeout, oversized or invalid JSON
};

enum class RequestKind { Validation, Snapshot };

struct Dispatch {
    RequestKind kind = RequestKind::Snapshot;
    std::uint16_t symbol = 0;
    std::uint64_t slot_value = 0;  // (epoch << 32) | request number; 0 for validation
    int weight = 0;
};

enum class Outcome {
    Deliver,        // 200: hand the body to the delivery callback
    Retry,          // requeued (cooldown or backoff)
    InvalidSymbol,  // validation 400 / -1121: usage error (exit 2)
    Unreachable,    // validation kept failing for validation_give_up (exit 1)
    Fatal,          // any other non-retryable 4xx (exit 1)
};

enum class DeliveryResult { Delivered, Discard, RingFull };

struct ValidationResult {
    enum class Kind { Ok, InvalidSymbol, Unreachable, Fatal } kind = Kind::Ok;
    std::string symbol;
    std::string message;
};

// The REST scheduler's decisions (design §6.3), with no I/O and no clock of its own: a token
// bucket at 50 % of the venue's weight budget, 4 dispatch slots held until a body is delivered
// or discarded, one latest-value slot per symbol, usage-header reconciliation, 429/418
// cooldowns and per-request backoff.
class DispatchPolicy {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int kDispatchSlots = 4;

    DispatchPolicy(Venue venue, const NetTuning& tuning);

    void want(RequestKind kind, std::uint16_t symbol, std::uint64_t slot_value,
              Clock::time_point now);
    std::optional<Dispatch> next_dispatch(Clock::time_point now, std::int64_t wall_now_s);
    // When next_dispatch could next succeed; time_point::max() when nothing is waiting.
    Clock::time_point next_wakeup(Clock::time_point now) const;

    Outcome on_response(std::uint16_t symbol, const HttpResult& result, Clock::time_point now,
                        std::int64_t wall_now_s);
    void on_delivery(std::uint16_t symbol, DeliveryResult result, Clock::time_point now);

    // Forgets everything queued or in flight (shutdown).
    void clear() noexcept;

    bool cooling_down(Clock::time_point now) const noexcept { return now < cooldown_until_; }
    int budget_per_minute() const noexcept { return budget_; }
    int weight(RequestKind kind) const noexcept;

private:
    struct Queued {
        RequestKind kind;
        std::uint16_t symbol;
        std::uint64_t value;
        Clock::time_point not_before;
    };
    struct SymbolState {
        bool queued = false;
        bool busy = false;  // in flight, or its body waits for delivery
        RequestKind busy_kind = RequestKind::Snapshot;
        std::uint64_t busy_value = 0;
        std::optional<std::uint64_t> newer;  // wanted while busy
        RequestKind newer_kind = RequestKind::Snapshot;
        unsigned failures = 0;
        std::optional<Clock::time_point> first_validation;
    };

    SymbolState& state(std::uint16_t symbol);
    void refill(Clock::time_point now) const;
    void enqueue(RequestKind kind, std::uint16_t symbol, std::uint64_t value,
                 Clock::time_point not_before, bool at_head);
    void release(std::uint16_t symbol, Clock::time_point now);
    std::chrono::milliseconds backoff(unsigned failures) const;

    Venue venue_;
    NetTuning tuning_;
    int budget_;
    mutable double tokens_;
    mutable Clock::time_point refilled_at_{};
    mutable bool refill_started_ = false;
    int in_use_ = 0;
    Clock::time_point cooldown_until_{};
    Clock::time_point paused_until_{};
    std::deque<Queued> queue_;
    std::vector<SymbolState> symbols_;
};

}  // namespace blc
