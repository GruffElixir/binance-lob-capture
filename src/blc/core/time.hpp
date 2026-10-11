#pragma once

#include <cstdint>
#include <string>

namespace blc {

inline constexpr std::int64_t kNanosPerSecond = 1'000'000'000;

struct SplitTime {
    std::int64_t sec;
    std::int32_t nsec;  // always in [0, 999 999 999]
};

// Floor division, so negative inputs still give a non-negative nanosecond remainder.
constexpr SplitTime split_ns(std::int64_t ns) noexcept {
    std::int64_t sec = ns / kNanosPerSecond;
    std::int64_t rem = ns % kNanosPerSecond;
    if (rem < 0) {
        rem += kNanosPerSecond;
        --sec;
    }
    return SplitTime{sec, static_cast<std::int32_t>(rem)};
}

// "YYYY-MM-DD" for the UTC date containing `ns` (nanoseconds since the Unix epoch).
std::string utc_date(std::int64_t ns);

// "YYYYMMDDTHHMMSSZ" for the UTC instant `ns`.
std::string utc_compact_stamp(std::int64_t ns);

// Wall-clock time (CLOCK_REALTIME) in nanoseconds since the Unix epoch.
std::int64_t wall_clock_ns() noexcept;

// Monotonic clock in nanoseconds (latency measurement only; never written to a CSV).
std::int64_t steady_clock_ns() noexcept;

}  // namespace blc
