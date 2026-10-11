#include "blc/net/dispatch_policy.hpp"

#include <algorithm>
#include <cmath>

namespace blc {

namespace {

constexpr std::int64_t kBinanceInvalidSymbol = -1121;

bool retryable_status(int status) noexcept {
    return status >= 500 || status < 400 || status == 429 || status == 418;
}

}  // namespace

DispatchPolicy::DispatchPolicy(Venue venue, const NetTuning& tuning)
    : venue_(venue),
      tuning_(tuning),
      budget_(venue == Venue::Spot ? 3000 : 1200),
      tokens_(static_cast<double>(budget_)) {}

int DispatchPolicy::weight(RequestKind kind) const noexcept {
    if (kind == RequestKind::Validation) {
        return venue_ == Venue::Spot ? 2 : 1;
    }
    return venue_ == Venue::Spot ? 50 : 20;
}

DispatchPolicy::SymbolState& DispatchPolicy::state(std::uint16_t symbol) {
    if (symbol >= symbols_.size()) {
        symbols_.resize(std::size_t{symbol} + 1);
    }
    return symbols_[symbol];
}

void DispatchPolicy::refill(Clock::time_point now) const {
    if (!refill_started_) {
        refill_started_ = true;
        refilled_at_ = now;
        return;
    }
    if (now <= refilled_at_) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - refilled_at_).count();
    tokens_ = std::min(static_cast<double>(budget_),
                       tokens_ + seconds * static_cast<double>(budget_) / 60.0);
    refilled_at_ = now;
}

std::chrono::milliseconds DispatchPolicy::backoff(unsigned failures) const {
    std::chrono::milliseconds d = tuning_.backoff_initial;
    for (unsigned i = 1; i < failures && d < tuning_.backoff_cap; ++i) {
        d *= 2;
    }
    return std::min(d, tuning_.backoff_cap);
}

void DispatchPolicy::enqueue(RequestKind kind, std::uint16_t symbol, std::uint64_t value,
                             Clock::time_point not_before, bool at_head) {
    SymbolState& s = state(symbol);
    s.queued = true;
    const Queued q{kind, symbol, value, not_before};
    if (at_head) {
        queue_.push_front(q);
    } else {
        queue_.push_back(q);
    }
}

void DispatchPolicy::want(RequestKind kind, std::uint16_t symbol, std::uint64_t slot_value,
                          Clock::time_point now) {
    SymbolState& s = state(symbol);
    if (kind == RequestKind::Validation && !s.first_validation) {
        s.first_validation = now;
    }
    if (s.queued) {
        for (Queued& q : queue_) {
            if (q.symbol == symbol) {
                q.kind = kind;
                q.value = slot_value;  // replaced in place: keeps its queue position
            }
        }
        return;
    }
    if (s.busy) {
        s.newer = slot_value;
        s.newer_kind = kind;
        return;
    }
    enqueue(kind, symbol, slot_value, now, false);
}

std::optional<Dispatch> DispatchPolicy::next_dispatch(Clock::time_point now,
                                                      std::int64_t /*wall_now_s*/) {
    refill(now);
    if (now < cooldown_until_ || now < paused_until_ || in_use_ >= kDispatchSlots) {
        return std::nullopt;
    }
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->not_before > now) {
            continue;  // backing off; later requests may go first
        }
        const int w = weight(it->kind);
        if (tokens_ < static_cast<double>(w)) {
            return std::nullopt;  // FIFO: the first eligible request waits for its tokens
        }
        tokens_ -= w;
        const Dispatch d{it->kind, it->symbol, it->value, w};
        SymbolState& s = state(it->symbol);
        s.queued = false;
        s.busy = true;
        s.busy_kind = it->kind;
        s.busy_value = it->value;
        queue_.erase(it);
        ++in_use_;
        return d;
    }
    return std::nullopt;
}

DispatchPolicy::Clock::time_point DispatchPolicy::next_wakeup(Clock::time_point now) const {
    if (queue_.empty()) {
        return Clock::time_point::max();
    }
    refill(now);
    Clock::time_point const at = std::max({now, cooldown_until_, paused_until_});
    if (in_use_ >= kDispatchSlots) {
        return Clock::time_point::max();  // a delivery or failure frees a slot first
    }
    Clock::time_point earliest = Clock::time_point::max();
    for (const Queued& q : queue_) {
        Clock::time_point t = std::max(at, q.not_before);
        const double missing = static_cast<double>(weight(q.kind)) - tokens_;
        if (missing > 0) {
            const double seconds = missing * 60.0 / static_cast<double>(budget_);
            t = std::max(t, refilled_at_ + std::chrono::duration_cast<Clock::duration>(
                                               std::chrono::duration<double>(seconds)));
        }
        earliest = std::min(earliest, t);
    }
    return earliest;
}

void DispatchPolicy::release(std::uint16_t symbol, Clock::time_point now) {
    SymbolState& s = state(symbol);
    if (s.busy) {
        s.busy = false;
        --in_use_;
    }
    if (s.newer) {
        enqueue(s.newer_kind, symbol, *s.newer, now, false);
        s.newer.reset();
    }
}

Outcome DispatchPolicy::on_response(std::uint16_t symbol, const HttpResult& r,
                                    Clock::time_point now, std::int64_t wall_now_s) {
    SymbolState& s = state(symbol);
    const RequestKind kind = s.busy_kind;

    // Reconciliation with the server's view of our usage (any response that carries it).
    if (r.used_weight_1m > 0 && budget_ - r.used_weight_1m < weight(RequestKind::Snapshot)) {
        const std::int64_t to_next_minute = 60 - ((wall_now_s % 60) + 60) % 60;
        paused_until_ = std::max(paused_until_, now + std::chrono::seconds(to_next_minute));
    }

    if (!r.transport_error && r.status == 200) {
        s.failures = 0;
        if (kind == RequestKind::Validation) {
            s.first_validation.reset();
        }
        return Outcome::Deliver;  // the dispatch slot is held until on_delivery
    }

    if (!r.transport_error && !retryable_status(r.status)) {
        const bool invalid_symbol = kind == RequestKind::Validation && r.status == 400 &&
                                    r.binance_error_code == kBinanceInvalidSymbol;
        s.newer.reset();
        release(symbol, now);
        return invalid_symbol ? Outcome::InvalidSymbol : Outcome::Fatal;
    }

    // Retryable: 429/418 cool everything down; others back off this request.
    const bool cooldown = !r.transport_error && (r.status == 429 || r.status == 418);
    if (cooldown) {
        const auto pause = r.retry_after_s > 0
                               ? std::chrono::milliseconds(std::chrono::seconds(r.retry_after_s))
                               : tuning_.default_retry_after;
        cooldown_until_ = std::max(cooldown_until_, now + pause);
    }
    if (kind == RequestKind::Validation && s.first_validation &&
        now - *s.first_validation >= tuning_.validation_give_up) {
        s.newer.reset();
        release(symbol, now);
        return Outcome::Unreachable;
    }
    ++s.failures;
    const std::uint64_t failed_value = s.busy_value;
    const RequestKind failed_kind = s.busy_kind;
    const bool has_newer = s.newer.has_value();
    release(symbol, now);  // queues the newer value instead, if there is one
    if (!has_newer) {
        enqueue(failed_kind, symbol, failed_value, cooldown ? now : now + backoff(s.failures),
                cooldown);
    }
    return Outcome::Retry;
}

void DispatchPolicy::on_delivery(std::uint16_t symbol, DeliveryResult result,
                                 Clock::time_point now) {
    if (result == DeliveryResult::RingFull) {
        return;  // the body keeps its dispatch slot while it waits for ring space
    }
    release(symbol, now);
}

void DispatchPolicy::clear() noexcept {
    queue_.clear();
    symbols_.clear();
    in_use_ = 0;
}

}  // namespace blc
