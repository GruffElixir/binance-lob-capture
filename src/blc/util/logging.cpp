#include "blc/util/logging.hpp"

#include "blc/core/time.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>

namespace blc {
namespace {

std::atomic<LogAllocationHook> g_hook{nullptr};
std::mutex g_write_mutex;

class HookScope {
public:
    HookScope() noexcept : hook_(g_hook.load(std::memory_order_acquire)) {
        if (hook_ != nullptr) {
            hook_(true);
        }
    }
    ~HookScope() {
        if (hook_ != nullptr) {
            hook_(false);
        }
    }
    HookScope(const HookScope&) = delete;
    HookScope& operator=(const HookScope&) = delete;

private:
    LogAllocationHook hook_;
};

std::string_view level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
    }
    return "info";
}

void write_prefix(std::ostream& os, LogLevel level) {
    const std::int64_t now = wall_clock_ns();
    const SplitTime t = split_ns(now);
    std::int64_t sod = t.sec % 86'400;
    if (sod < 0) {
        sod += 86'400;
    }
    char buf[48];
    std::snprintf(buf, sizeof buf, "T%02d:%02d:%02d.%03dZ ", static_cast<int>(sod / 3600),
                  static_cast<int>((sod / 60) % 60), static_cast<int>(sod % 60),
                  static_cast<int>(t.nsec / 1'000'000));
    os << utc_date(now) << buf << level_name(level) << ' ';
}

void emit(const std::string& line) noexcept {
    const std::lock_guard<std::mutex> lock(g_write_mutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

}  // namespace

void set_log_allocation_hook(LogAllocationHook hook) noexcept {
    g_hook.store(hook, std::memory_order_release);
}

void log(LogLevel level, std::string_view message) noexcept {
    const HookScope scope;
    try {
        std::ostringstream os;
        write_prefix(os, level);
        os << message << '\n';
        emit(os.str());
    } catch (...) {
        // Logging must never take the process down.
    }
}

void log_formatted(LogLevel level, LogFormatter formatter, const void* context) noexcept {
    const HookScope scope;
    try {
        std::ostringstream os;
        write_prefix(os, level);
        try {
            formatter(os, context);
        } catch (const std::exception& e) {
            os << "<log formatting failed: " << e.what() << '>';
        } catch (...) {
            os << "<log formatting failed>";
        }
        os << '\n';
        emit(os.str());
    } catch (...) {
    }
}

}  // namespace blc
