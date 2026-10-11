#pragma once

#include "blc/core/symbols.hpp"
#include "blc/core/types.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace blc {

inline constexpr std::int64_t kMaxDurationSeconds = 2'678'400;  // 31 days
inline constexpr int kMaxSymbolsPerConnection = 341;            // 1023 of 1024 streams
inline constexpr int kMaxConnections = 64;

struct CaptureOptions {
    Venue venue = Venue::Spot;
    std::vector<SymbolSpec> symbols;
    std::filesystem::path output_dir = ".";
    std::int64_t duration_s = 0;  // 0 = until SIGINT/SIGTERM
    bool trade_rows = true;
    int symbols_per_connection = 50;
    int stats_interval_s = 10;  // 0 = final summary only
    int fault_reconnect_every_s = 0;
};

struct ReplayOptions {
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path output_dir = "./replay_output";
    bool trade_rows = true;
    int stats_interval_s = 10;
    // Library-only benchmark hook. Empty in normal CLI replay; includes CSV decoding,
    // attached snapshots and processing for each market-data row.
    std::function<void(std::int64_t)> record_latency_observer;
};

enum class Mode { Capture, Replay, Help, Version };

struct Options {
    Mode mode = Mode::Capture;
    CaptureOptions capture;
    ReplayOptions replay;
};

// Parses the command line (design §2). Throws UsageError for anything invalid.
Options parse_options(int argc, const char* const* argv);

std::string usage_text();

}  // namespace blc
