#include "blc/net/rest_scheduler.hpp"

#include "blc/core/time.hpp"
#include "blc/util/logging.hpp"

#include <algorithm>

namespace blc {

namespace {

using Clock = DispatchPolicy::Clock;

std::int64_t wall_seconds() noexcept {
    return wall_clock_ns() / kNanosPerSecond;
}

std::string describe(const HttpResult& r) {
    if (r.transport_error) {
        return "transport error";
    }
    std::string s = "HTTP " + std::to_string(r.status);
    if (!r.body.empty()) {
        s += ": " + r.body.substr(0, 512);
    }
    return s;
}

}  // namespace

RestScheduler::RestScheduler(boost::asio::io_context& io, Venue venue,
                             const VenueEndpoints& endpoints, const NetTuning& tuning,
                             RestTransport& transport, IoMetrics& metrics)
    : io_(io),
      venue_(venue),
      endpoints_(endpoints),
      tuning_(tuning),
      transport_(transport),
      metrics_(metrics),
      policy_(venue, tuning),
      timer_(io) {}

RestScheduler::~RestScheduler() {
    stop();
}

void RestScheduler::validate_symbols(const std::vector<SymbolSpec>& symbols,
                                     std::function<void(ValidationResult)> done) {
    symbols_ = symbols;
    validation_done_ = std::move(done);
    validations_pending_ = symbols.size();
    if (symbols.empty()) {
        report_validation(ValidationResult{});
        return;
    }
    const auto now = Clock::now();
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        policy_.want(RequestKind::Validation, static_cast<std::uint16_t>(i), 0, now);
    }
    pump();
}

void RestScheduler::want(std::uint16_t symbol, std::uint64_t slot_value) {
    if (stopping_ || symbol >= symbols_.size()) {
        return;
    }
    policy_.want(RequestKind::Snapshot, symbol, slot_value, Clock::now());
    pump();
}

void RestScheduler::stop() {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    timer_.cancel();
    transport_.cancel_all();
    waiting_.clear();
    policy_.clear();
}

void RestScheduler::pump() {
    if (stopping_) {
        return;
    }
    const auto now = Clock::now();
    const std::int64_t wall = wall_seconds();
    while (auto d = policy_.next_dispatch(now, wall)) {
        IoMetrics::add(metrics_.rest_requests);
        IoMetrics::add(metrics_.rest_weight_used, static_cast<std::uint64_t>(d->weight));
        const std::string& name = symbols_.at(d->symbol).upper;
        std::string target = d->kind == RequestKind::Validation ? ticker_target(endpoints_, name)
                                                                : depth_target(endpoints_, name);
        std::weak_ptr<char> life = life_;
        const Dispatch dispatch = *d;
        transport_.get(endpoints_.rest, std::move(target),
                       [this, life, dispatch](HttpResult result) {
                           if (life.expired()) {
                               return;
                           }
                           complete(dispatch, std::move(result));
                       });
    }
    arm_timer();
}

void RestScheduler::arm_timer() {
    if (stopping_) {
        return;
    }
    const auto now = Clock::now();
    Clock::time_point wake = policy_.next_wakeup(now);
    for (const Waiting& w : waiting_) {
        wake = std::min(wake, w.retry_at);
    }
    if (wake == Clock::time_point::max()) {
        timer_.cancel();
        return;
    }
    timer_.expires_at(wake);
    std::weak_ptr<char> life = life_;
    timer_.async_wait([this, life](const boost::system::error_code& ec) {
        if (ec || life.expired() || stopping_) {
            return;
        }
        on_timer();
    });
}

void RestScheduler::on_timer() {
    const auto now = Clock::now();
    std::vector<Waiting> due;
    for (auto it = waiting_.begin(); it != waiting_.end();) {
        if (it->retry_at <= now) {
            due.push_back(std::move(*it));
            it = waiting_.erase(it);
        } else {
            ++it;
        }
    }
    for (Waiting& w : due) {
        deliver(w.symbol, w.epoch, std::move(w.result));
    }
    pump();
}

void RestScheduler::complete(const Dispatch& d, HttpResult result) {
    if (stopping_) {
        return;
    }
    const auto now = Clock::now();
    const Outcome outcome = policy_.on_response(d.symbol, result, now, wall_seconds());
    if (outcome == Outcome::Retry) {
        IoMetrics::add(metrics_.rest_failures);
        if (!result.transport_error && (result.status == 429 || result.status == 418)) {
            IoMetrics::add(metrics_.rest_cooldowns);
            if (result.status == 418) {
                log(LogLevel::Error,
                    "REST 418: this IP is banned by Binance; pausing all "
                    "requests (Retry-After " +
                        std::to_string(result.retry_after_s) + " s)");
            }
        }
    }
    if (d.kind == RequestKind::Validation) {
        complete_validation(d, outcome, result);
    } else if (outcome == Outcome::Deliver) {
        deliver(d.symbol, static_cast<std::uint32_t>(d.slot_value >> 32), std::move(result));
    } else if (outcome == Outcome::Fatal || outcome == Outcome::InvalidSymbol) {
        if (!fatal_reported_) {
            fatal_reported_ = true;
            const std::string message = "snapshot request for " + symbols_.at(d.symbol).upper +
                                        " failed: " + describe(result);
            if (fatal_) {
                fatal_(message);
            }
        }
    }
    pump();
}

void RestScheduler::complete_validation(const Dispatch& d, Outcome outcome,
                                        const HttpResult& result) {
    const std::string& name = symbols_.at(d.symbol).upper;
    switch (outcome) {
        case Outcome::Deliver:
            policy_.on_delivery(d.symbol, DeliveryResult::Delivered, Clock::now());
            if (validations_pending_ > 0 && --validations_pending_ == 0) {
                report_validation(ValidationResult{});
            }
            break;
        case Outcome::InvalidSymbol:
            report_validation({ValidationResult::Kind::InvalidSymbol, name,
                               "invalid symbol " + name + " (Binance -1121)"});
            break;
        case Outcome::Unreachable:
            report_validation({ValidationResult::Kind::Unreachable, name,
                               "cannot reach REST endpoint " + endpoints_.rest.host + " (" +
                                   describe(result) + ")"});
            break;
        case Outcome::Fatal:
            report_validation({ValidationResult::Kind::Fatal, name,
                               "symbol validation for " + name + " failed: " + describe(result)});
            break;
        case Outcome::Retry:
            break;
    }
}

void RestScheduler::report_validation(ValidationResult result) {
    if (!validation_done_) {
        return;
    }
    auto done = std::move(validation_done_);
    validation_done_ = nullptr;
    done(std::move(result));
}

void RestScheduler::deliver(std::uint16_t symbol, std::uint32_t epoch, HttpResult result) {
    if (stopping_) {
        return;
    }
    const DeliveryResult r = delivery_ ? delivery_(symbol, epoch, result) : DeliveryResult::Discard;
    const auto now = Clock::now();
    policy_.on_delivery(symbol, r, now);
    if (r == DeliveryResult::Discard) {
        IoMetrics::add(metrics_.snapshots_discarded);
    } else if (r == DeliveryResult::RingFull) {
        waiting_.push_back(Waiting{symbol, epoch, std::move(result), now + tuning_.ring_retry});
    }
    arm_timer();
}

}  // namespace blc
