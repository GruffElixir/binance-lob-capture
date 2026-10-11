#include "blc/net/attempt_limiter.hpp"

#include <algorithm>
#include <cmath>

namespace blc {

AttemptLimiter::AttemptLimiter(std::size_t max_attempts, std::chrono::seconds window)
    : max_attempts_(max_attempts), window_(window) {}

void AttemptLimiter::expire(std::chrono::steady_clock::time_point now) {
    while (!taken_.empty() && taken_.front() + window_ <= now) {
        taken_.pop_front();
    }
}

bool AttemptLimiter::try_take(std::chrono::steady_clock::time_point now) {
    expire(now);
    if (taken_.size() >= max_attempts_) {
        return false;
    }
    taken_.push_back(now);
    return true;
}

std::chrono::steady_clock::time_point AttemptLimiter::next_available(
    std::chrono::steady_clock::time_point now) const {
    std::size_t live = 0;
    for (const auto& t : taken_) {
        live += t + window_ > now ? 1u : 0u;
    }
    if (live < max_attempts_) {
        return now;
    }
    // The (live - max + 1)-th oldest live attempt must expire first.
    const std::size_t first_live = taken_.size() - live;
    return taken_[first_live + (live - max_attempts_)] + window_;
}

Backoff::Backoff(std::chrono::milliseconds initial, std::chrono::milliseconds cap, double jitter,
                 std::uint32_t seed)
    : initial_(initial), cap_(cap), jitter_(jitter), current_(initial), rng_(seed) {}

std::chrono::milliseconds Backoff::next() noexcept {
    const std::chrono::milliseconds base = current_;
    current_ = std::min(cap_, current_ * 2);
    if (jitter_ <= 0.0) {
        return base;
    }
    std::uniform_real_distribution<double> factor(1.0 - jitter_, 1.0 + jitter_);
    return std::chrono::milliseconds(
        static_cast<std::int64_t>(std::llround(static_cast<double>(base.count()) * factor(rng_))));
}

}  // namespace blc
