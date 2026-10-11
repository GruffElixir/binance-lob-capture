#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <random>

namespace blc {

// Process-wide sliding window on connection attempts (design §6.2): at most `max_attempts` in
// any `window`. Every attempt — startup, reconnect, fault reconnect, loss recovery — takes one.
class AttemptLimiter {
public:
    explicit AttemptLimiter(std::size_t max_attempts = 200,
                            std::chrono::seconds window = std::chrono::minutes(5));

    bool try_take(std::chrono::steady_clock::time_point now);
    // The earliest time try_take can succeed (`now` if it can already).
    std::chrono::steady_clock::time_point next_available(
        std::chrono::steady_clock::time_point now) const;

private:
    void expire(std::chrono::steady_clock::time_point now);

    std::size_t max_attempts_;
    std::chrono::steady_clock::duration window_;
    std::deque<std::chrono::steady_clock::time_point> taken_;
};

// Reconnect backoff: doubles from `initial` up to `cap`, each value scaled by a uniform factor
// in [1 - jitter, 1 + jitter].
class Backoff {
public:
    Backoff(std::chrono::milliseconds initial, std::chrono::milliseconds cap, double jitter,
            std::uint32_t seed);

    std::chrono::milliseconds next() noexcept;
    void reset() noexcept { current_ = initial_; }

private:
    std::chrono::milliseconds initial_;
    std::chrono::milliseconds cap_;
    double jitter_;
    std::chrono::milliseconds current_;
    std::mt19937 rng_;
};

}  // namespace blc
