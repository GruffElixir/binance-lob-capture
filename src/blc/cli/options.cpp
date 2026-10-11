#include "blc/cli/options.hpp"

#include "blc/core/errors.hpp"

#include <charconv>
#include <optional>
#include <string_view>

namespace blc {
namespace {

std::int64_t parse_int(std::string_view flag, std::string_view value, std::int64_t min,
                       std::int64_t max) {
    std::int64_t v = 0;
    const auto* end = value.data() + value.size();
    const auto res = std::from_chars(value.data(), end, v);
    if (value.empty() || res.ec != std::errc{} || res.ptr != end) {
        throw UsageError(std::string(flag) + ": expected an integer, got '" + std::string(value) +
                         "'");
    }
    if (v < min || v > max) {
        throw UsageError(std::string(flag) + ": " + std::string(value) + " is out of range [" +
                         std::to_string(min) + ", " + std::to_string(max) + "]");
    }
    return v;
}

}  // namespace

Options parse_options(int argc, const char* const* argv) {
    Options o;
    std::optional<std::string> symbols_arg;
    bool saw_replay = false;
    bool saw_capture_only = false;
    bool saw_output_dir = false;
    std::optional<std::filesystem::path> output_dir;
    bool trade_rows = true;
    std::optional<int> stats_interval;

    if (argc <= 1) {
        throw UsageError("no arguments; see --help");
    }

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        std::string_view inline_value;
        bool has_inline = false;
        if (arg.size() > 2 && arg.substr(0, 2) == "--") {
            const std::size_t eq = arg.find('=');
            if (eq != std::string_view::npos) {
                inline_value = arg.substr(eq + 1);
                arg = arg.substr(0, eq);
                has_inline = true;
            }
        }
        const auto value = [&]() -> std::string_view {
            if (has_inline) {
                return inline_value;
            }
            if (i + 1 >= argc) {
                throw UsageError(std::string(arg) + ": missing value");
            }
            return std::string_view{argv[++i]};
        };
        const auto no_value = [&] {
            if (has_inline) {
                throw UsageError(std::string(arg) + " takes no value");
            }
        };

        if (arg == "--help" || arg == "-h") {
            no_value();
            o.mode = Mode::Help;
            return o;
        }
        if (arg == "--version" || arg == "-V") {
            no_value();
            o.mode = Mode::Version;
            return o;
        }
        if (arg == "--venue") {
            const auto v = parse_venue(value());
            if (!v) {
                throw UsageError("--venue: expected 'spot' or 'usdm'");
            }
            o.capture.venue = *v;
            saw_capture_only = true;
        } else if (arg == "--symbols") {
            symbols_arg = std::string(value());
            saw_capture_only = true;
        } else if (arg == "--output-dir") {
            output_dir = std::filesystem::path(std::string(value()));
            saw_output_dir = true;
        } else if (arg == "--duration") {
            o.capture.duration_s = parse_int(arg, value(), 0, kMaxDurationSeconds);
            saw_capture_only = true;
        } else if (arg == "--no-trade-rows") {
            no_value();
            trade_rows = false;
        } else if (arg == "--symbols-per-connection") {
            o.capture.symbols_per_connection =
                static_cast<int>(parse_int(arg, value(), 1, kMaxSymbolsPerConnection));
            saw_capture_only = true;
        } else if (arg == "--stats-interval") {
            stats_interval = static_cast<int>(parse_int(arg, value(), 0, 86'400));
        } else if (arg == "--fault-reconnect-every") {
            o.capture.fault_reconnect_every_s =
                static_cast<int>(parse_int(arg, value(), 0, 86'400));
            saw_capture_only = true;
        } else if (arg == "--replay") {
            saw_replay = true;
            if (has_inline) {
                o.replay.inputs.emplace_back(std::string(inline_value));
            }
            while (i + 1 < argc && std::string_view{argv[i + 1]}.substr(0, 2) != "--") {
                o.replay.inputs.emplace_back(std::string(argv[++i]));
            }
        } else {
            throw UsageError("unknown option '" + std::string(arg) + "'; see --help");
        }
    }

    if (saw_replay) {
        if (saw_capture_only) {
            throw UsageError("--replay cannot be combined with capture options");
        }
        if (o.replay.inputs.empty()) {
            throw UsageError("--replay: at least one market-data CSV is required");
        }
        o.mode = Mode::Replay;
        o.replay.trade_rows = trade_rows;
        if (saw_output_dir) {
            o.replay.output_dir = *output_dir;
        }
        if (stats_interval) {
            o.replay.stats_interval_s = *stats_interval;
        }
        return o;
    }

    if (!symbols_arg) {
        throw UsageError("--symbols is required (e.g. --symbols BTCUSDT); see --help");
    }
    o.mode = Mode::Capture;
    o.capture.symbols = parse_symbol_list(o.capture.venue, *symbols_arg);
    o.capture.trade_rows = trade_rows;
    if (saw_output_dir) {
        o.capture.output_dir = *output_dir;
    }
    if (stats_interval) {
        o.capture.stats_interval_s = *stats_interval;
    }
    const auto n = static_cast<long>(o.capture.symbols.size());
    const long per = o.capture.symbols_per_connection;
    const long connections = (n + per - 1) / per;
    if (connections > kMaxConnections) {
        throw UsageError(std::to_string(n) + " symbols at " + std::to_string(per) +
                         " per connection need " + std::to_string(connections) +
                         " connections; the maximum is " + std::to_string(kMaxConnections));
    }
    return o;
}

std::string usage_text() {
    return R"(Usage:
  binance_capture [--venue spot|usdm] --symbols LIST [--output-dir DIR] [--duration SECONDS]
                  [--no-trade-rows] [--symbols-per-connection N] [--stats-interval SECONDS]
                  [--fault-reconnect-every SECONDS]
  binance_capture --replay FILE [FILE...] [--output-dir DIR] [--no-trade-rows]
                  [--stats-interval SECONDS]
  binance_capture --help | --version

Capture options:
  --venue spot|usdm              Binance spot or USD-M futures (default: spot)
  --symbols LIST                 Comma-separated symbols, e.g. BTCUSDT,ETHUSDT (required)
  --output-dir DIR               Output directory (default: . ; replay: ./replay_output)
  --duration SECONDS             Stop after SECONDS (0 = until SIGINT/SIGTERM; max 2678400)
  --no-trade-rows                Do not emit order-book rows for trades
  --symbols-per-connection N     Symbols per WebSocket connection (default: 50, max: 341)
  --stats-interval SECONDS       Seconds between stats lines on stderr (0 = summary only)
  --fault-reconnect-every SECONDS  Testing aid: force a reconnect every SECONDS

Replay:
  --replay FILE...               Rebuild *_orderbook.csv from market_data_*.csv (no network)

Exit codes: 0 success, 1 runtime failure, 2 usage error.
)";
}

}  // namespace blc
