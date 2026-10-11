#include "blc/cli/options.hpp"
#include "blc/core/errors.hpp"
#include "blc/util/logging.hpp"

#include <initializer_list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

Options parse(std::initializer_list<std::string> args) {
    std::vector<std::string> storage{"binance_capture"};
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<const char*> argv;
    for (const auto& s : storage) {
        argv.push_back(s.c_str());
    }
    return parse_options(static_cast<int>(argv.size()), argv.data());
}

std::string many_symbols(int n) {
    std::string list;
    for (int i = 0; i < n; ++i) {
        list += (i == 0 ? "" : ",") + std::string("S") + std::to_string(i);
    }
    return list;
}

TEST(Options, Defaults) {
    const auto o = parse({"--symbols", "BTCUSDT"});
    EXPECT_EQ(o.mode, Mode::Capture);
    EXPECT_EQ(o.capture.output_dir, ".");
    EXPECT_EQ(o.capture.venue, Venue::Spot);
    EXPECT_EQ(o.capture.symbols_per_connection, 50);
    EXPECT_TRUE(o.capture.trade_rows);
    EXPECT_EQ(o.capture.duration_s, 0);
    EXPECT_EQ(o.capture.stats_interval_s, 10);
    EXPECT_EQ(o.capture.fault_reconnect_every_s, 0);
    ASSERT_EQ(o.capture.symbols.size(), 1u);
    EXPECT_EQ(o.capture.symbols[0].upper, "BTCUSDT");
}

TEST(Options, EqualsForm) {
    const auto o = parse({"--venue=usdm", "--symbols=BTCUSDT", "--duration=300"});
    EXPECT_EQ(o.capture.venue, Venue::Usdm);
    EXPECT_EQ(o.capture.duration_s, 300);
    EXPECT_EQ(o.capture.symbols[0].id, 1567771867);  // ids use the parsed venue
}

TEST(Options, VenueAfterSymbolsStillApplies) {
    const auto o = parse({"--symbols", "BTCUSDT", "--venue", "usdm"});
    EXPECT_EQ(o.capture.symbols[0].id, 1567771867);
}

TEST(Options, ReplayDefaults) {
    const auto o = parse({"--replay", "x.csv", "y.csv"});
    EXPECT_EQ(o.mode, Mode::Replay);
    EXPECT_EQ(o.replay.inputs.size(), 2u);
    EXPECT_EQ(o.replay.output_dir, "./replay_output");
}

TEST(Options, ReplayWithOutputDirAndFlags) {
    const auto o = parse({"--replay", "a.csv", "--output-dir", "out", "--no-trade-rows"});
    EXPECT_EQ(o.replay.inputs.size(), 1u);
    EXPECT_EQ(o.replay.output_dir, "out");
    EXPECT_FALSE(o.replay.trade_rows);
}

TEST(Options, AllFlagsParsed) {
    const auto o = parse({"--symbols", "BTCUSDT", "--no-trade-rows", "--stats-interval", "0",
                          "--fault-reconnect-every", "30", "--symbols-per-connection", "341",
                          "--output-dir", "./output"});
    EXPECT_FALSE(o.capture.trade_rows);
    EXPECT_EQ(o.capture.stats_interval_s, 0);
    EXPECT_EQ(o.capture.fault_reconnect_every_s, 30);
    EXPECT_EQ(o.capture.symbols_per_connection, 341);
    EXPECT_EQ(o.capture.output_dir, "./output");
    EXPECT_EQ(parse({"--help"}).mode, Mode::Help);
    EXPECT_EQ(parse({"--version"}).mode, Mode::Version);
    EXPECT_EQ(parse({"-V"}).mode, Mode::Version);
}

TEST(Options, UsageErrors) {
    const std::vector<std::vector<std::string>> bad = {
        {"--symbols", "BTCUSDT", "--symbols-per-connection", "342"},
        {"--symbols", many_symbols(257)},
        {"--symbols", "BTCUSDT", "--stats-interval", "-1"},
        {"--symbols", "BTCUSDT", "--fault-reconnect-every", "-1"},
        {"--symbols", "BTCUSDT", "--bogus"},
        {"--symbols", "BTCUSDT", "--venue", "coin"},
        {"--symbols", "BTCUSDT", "--duration", "-1"},
        {"--symbols", "BTCUSDT", "--duration", "2678401"},
        {"--symbols", "BTCUSDT", "--duration", "12x"},
        {"--venue", "spot"},
        {"--symbols", "BTCUSDT", "--symbols-per-connection", "0"},
        {"--symbols", many_symbols(256), "--symbols-per-connection", "1"},
        {"--replay"},
        {"--replay", "a.csv", "--symbols", "BTCUSDT"},
        {"--symbols"},
        {"--symbols", "BTCUSDT", "--output-dir"},
        {},
    };
    for (const auto& args : bad) {
        std::vector<std::string> storage{"binance_capture"};
        storage.insert(storage.end(), args.begin(), args.end());
        std::vector<const char*> argv;
        for (const auto& s : storage) {
            argv.push_back(s.c_str());
        }
        std::string joined;
        for (const auto& a : args) {
            joined += a.substr(0, 40) + " ";
        }
        EXPECT_THROW(parse_options(static_cast<int>(argv.size()), argv.data()), UsageError)
            << joined;
    }
}

TEST(Options, ConnectionCapBoundary) {
    // 256 symbols at 4 per connection = 64 connections: allowed.
    EXPECT_NO_THROW(parse({"--symbols", many_symbols(256), "--symbols-per-connection", "4"}));
    EXPECT_THROW(parse({"--symbols", many_symbols(256), "--symbols-per-connection", "3"}),
                 UsageError);
}

TEST(Options, DurationMaximumAccepted) {
    EXPECT_EQ(parse({"--symbols", "BTCUSDT", "--duration", "2678400"}).capture.duration_s, 2678400);
}

TEST(Options, UsageTextMentionsEveryFlag) {
    const std::string u = usage_text();
    for (const char* flag : {"--venue", "--symbols", "--output-dir", "--duration",
                             "--no-trade-rows", "--symbols-per-connection", "--stats-interval",
                             "--fault-reconnect-every", "--replay", "--help", "--version"}) {
        EXPECT_NE(u.find(flag), std::string::npos) << flag;
    }
}

TEST(Logging, RateLimitedLogFirstTenThenEveryThousandth) {
    RateLimitedLog r;
    int logged = 0;
    for (int i = 0; i < 2010; ++i) {
        logged += r.should_log() ? 1 : 0;
    }
    EXPECT_EQ(logged, 12);  // events 1..10, 1000, 2000
}

int g_suspends = 0;
int g_resumes = 0;
void count_hook(bool suspend) noexcept {
    (suspend ? g_suspends : g_resumes) += 1;
}

void throwing_formatter(std::ostream&, const void*) {
    throw std::runtime_error("boom");
}

void context_formatter(std::ostream& os, const void* ctx) {
    os << "value=" << *static_cast<const int*>(ctx);
}

TEST(Logging, HookWrapsFormattingAndSurvivesExceptions) {
    g_suspends = g_resumes = 0;
    set_log_allocation_hook(&count_hook);
    log(LogLevel::Info, "hello");
    const int v = 42;
    log_formatted(LogLevel::Warn, &context_formatter, &v);
    log_formatted(LogLevel::Error, &throwing_formatter, nullptr);  // must not throw
    set_log_allocation_hook(nullptr);
    EXPECT_EQ(g_suspends, 3);
    EXPECT_EQ(g_resumes, 3);
}

}  // namespace
}  // namespace blc
