#include "blc/core/errors.hpp"
#include "blc/core/symbols.hpp"
#include "blc/net/endpoints.hpp"
#include "blc/parse/stream_table.hpp"

#include <string>

#include <gtest/gtest.h>

namespace blc {
namespace {

TEST(Endpoints, SpotValues) {
    const VenueEndpoints e = default_endpoints(Venue::Spot);
    EXPECT_EQ(e.ws.host, "stream.binance.com");
    EXPECT_EQ(e.ws.port, "9443");
    EXPECT_EQ(e.ws.target_prefix, "/stream?streams=");
    EXPECT_EQ(e.rest.host, "api.binance.com");
    EXPECT_EQ(e.rest.port, "443");
    EXPECT_EQ(e.depth_path, "/api/v3/depth");
    EXPECT_EQ(e.ticker_path, "/api/v3/ticker/price");
}

TEST(Endpoints, UsdmValues) {
    const VenueEndpoints e = default_endpoints(Venue::Usdm);
    EXPECT_EQ(e.ws.host, "fstream.binance.com");
    EXPECT_EQ(e.ws.port, "443");
    EXPECT_EQ(e.ws.target_prefix, "/public/stream?streams=");
    EXPECT_EQ(e.rest.host, "fapi.binance.com");
    EXPECT_EQ(e.rest.port, "443");
    EXPECT_EQ(e.depth_path, "/fapi/v1/depth");
    EXPECT_EQ(e.ticker_path, "/fapi/v1/ticker/price");
}

TEST(Endpoints, Targets) {
    const auto symbols = parse_symbol_list(Venue::Spot, "BTCUSDT");
    const StreamTable table(symbols, 0);
    EXPECT_EQ(ws_target(default_endpoints(Venue::Spot), table),
              "/stream?streams=btcusdt@depth@100ms/btcusdt@depth5@100ms/btcusdt@trade");
    EXPECT_EQ(ws_target(default_endpoints(Venue::Usdm), table),
              "/public/stream?streams=btcusdt@depth@100ms/btcusdt@depth5@100ms/btcusdt@trade");
    EXPECT_EQ(depth_target(default_endpoints(Venue::Spot), "BTCUSDT"),
              "/api/v3/depth?symbol=BTCUSDT&limit=1000");
    EXPECT_EQ(depth_target(default_endpoints(Venue::Usdm), "BTCUSDT"),
              "/fapi/v1/depth?symbol=BTCUSDT&limit=1000");
    EXPECT_EQ(ticker_target(default_endpoints(Venue::Spot), "BTCUSDT"),
              "/api/v3/ticker/price?symbol=BTCUSDT");
}

TEST(Endpoints, TargetTooLongIsUsageError) {
    std::string list;
    for (int i = 0; i < 120; ++i) {
        std::string s = "S" + std::to_string(i);
        s.resize(20, 'X');
        list += (list.empty() ? "" : ",") + s;
    }
    const auto symbols = parse_symbol_list(Venue::Spot, list);
    const StreamTable table(symbols, 0);
    EXPECT_THROW(ws_target(default_endpoints(Venue::Spot), table), UsageError);
}

TEST(Endpoints, Shards) {
    const auto shards = make_shards(256, 50);
    ASSERT_EQ(shards.size(), 6u);
    EXPECT_EQ(shards[0].size(), 50u);
    EXPECT_EQ(shards[5].size(), 6u);
    EXPECT_EQ(shards[0][0], 0);
    EXPECT_EQ(shards[5][5], 255);
    const auto one = make_shards(3, 341);
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0].size(), 3u);
}

TEST(NetTuning, DefaultsAreTheSpecValues) {
    const NetTuning t;
    EXPECT_EQ(t.limiter_attempts, 200u);
    EXPECT_EQ(t.limiter_window, std::chrono::minutes(5));
    EXPECT_EQ(t.backoff_initial, std::chrono::milliseconds(500));
    EXPECT_EQ(t.backoff_cap, std::chrono::seconds(30));
    EXPECT_DOUBLE_EQ(t.backoff_jitter, 0.2);
    EXPECT_EQ(t.backoff_reset_after, std::chrono::seconds(60));
    EXPECT_EQ(t.rest_timeout, std::chrono::seconds(10));
    EXPECT_EQ(t.validation_give_up, std::chrono::seconds(30));
    EXPECT_EQ(t.default_retry_after, std::chrono::seconds(60));
    EXPECT_EQ(t.ws_handshake_timeout, std::chrono::seconds(10));
    EXPECT_EQ(t.ws_idle_timeout, std::chrono::seconds(30));
    EXPECT_EQ(t.close_timeout, std::chrono::seconds(2));
    EXPECT_EQ(t.shutdown_deadline, std::chrono::seconds(3));
    EXPECT_EQ(t.poll_interval, std::chrono::milliseconds(10));
    EXPECT_EQ(t.ring_retry, std::chrono::milliseconds(10));
}

}  // namespace
}  // namespace blc
