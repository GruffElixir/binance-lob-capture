#include "blc/core/time.hpp"

#include "blc/core/errors.hpp"

#include <array>
#include <chrono>
#include <cstdio>

namespace blc {
namespace {

struct CivilDate {
    std::int64_t year;
    unsigned month;
    unsigned day;
};

// Howard Hinnant's civil_from_days: proleptic Gregorian date for days since 1970-01-01.
CivilDate civil_from_days(std::int64_t z) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return CivilDate{m <= 2 ? y + 1 : y, m, d};
}

struct UtcParts {
    CivilDate date;
    unsigned hour;
    unsigned minute;
    unsigned second;
};

UtcParts to_utc(std::int64_t ns) noexcept {
    const SplitTime t = split_ns(ns);
    constexpr std::int64_t kSecondsPerDay = 86'400;
    std::int64_t days = t.sec / kSecondsPerDay;
    std::int64_t secs = t.sec % kSecondsPerDay;
    if (secs < 0) {
        secs += kSecondsPerDay;
        --days;
    }
    const auto s = static_cast<unsigned>(secs);
    return UtcParts{civil_from_days(days), s / 3600, (s / 60) % 60, s % 60};
}

}  // namespace

std::string utc_date(std::int64_t ns) {
    const UtcParts p = to_utc(ns);
    std::array<char, 32> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%04lld-%02u-%02u",
                                static_cast<long long>(p.date.year), p.date.month, p.date.day);
    if (n < 0 || static_cast<std::size_t>(n) >= buf.size())
        throw IoError("cannot format UTC date");
    return buf.data();
}

std::string utc_compact_stamp(std::int64_t ns) {
    const UtcParts p = to_utc(ns);
    std::array<char, 40> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%04lld%02u%02uT%02u%02u%02uZ",
                                static_cast<long long>(p.date.year), p.date.month, p.date.day,
                                p.hour, p.minute, p.second);
    if (n < 0 || static_cast<std::size_t>(n) >= buf.size())
        throw IoError("cannot format UTC stamp");
    return buf.data();
}

std::int64_t wall_clock_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

std::int64_t steady_clock_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace blc
