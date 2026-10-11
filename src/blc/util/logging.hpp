#pragma once

#include <cstdint>
#include <iosfwd>
#include <string_view>

namespace blc {

enum class LogLevel { Info, Warn, Error };

// Writes "<ISO-8601 UTC> <LEVEL> <message>\n" to stderr as one write (thread-safe).
void log(LogLevel level, std::string_view message) noexcept;

// Formats a message inside the logger, so callers never build strings on hot paths.
// `formatter` runs synchronously; exceptions it throws are caught and reported.
using LogFormatter = void (*)(std::ostream&, const void* context);
void log_formatted(LogLevel level, LogFormatter formatter, const void* context) noexcept;

// Test hook: called with true before the logger formats/allocates and false afterwards.
using LogAllocationHook = void (*)(bool suspend) noexcept;
void set_log_allocation_hook(LogAllocationHook hook) noexcept;

// Rate limiter for repeated errors: allows the first 10 events, then every 1000th.
class RateLimitedLog {
public:
    bool should_log() noexcept {
        ++count_;
        return count_ <= 10 || count_ % 1000 == 0;
    }
    std::uint64_t count() const noexcept { return count_; }

private:
    std::uint64_t count_ = 0;
};

}  // namespace blc
