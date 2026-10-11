#pragma once

#include "blc/core/types.hpp"
#include "blc/parse/stream_table.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace blc {

struct Endpoint {
    std::string host;
    std::string port;
    std::string target_prefix;  // WebSocket: "/stream?streams="; REST: empty
};

struct VenueEndpoints {
    Endpoint ws;
    Endpoint rest;
    std::string depth_path;
    std::string ticker_path;
};

// The public Binance endpoints (design §6.2, §6.3).
VenueEndpoints default_endpoints(Venue venue);

// Splits symbols 0..n-1 into consecutive shards of at most `per_connection` symbols.
std::vector<std::vector<std::uint16_t>> make_shards(std::size_t n_symbols, int per_connection);

inline constexpr std::size_t kMaxTargetBytes = 8192;

// "<prefix><sym>@depth@100ms/<sym>@depth5@100ms/<sym>@trade/..." — throws UsageError above
// kMaxTargetBytes (lower --symbols-per-connection).
std::string ws_target(const VenueEndpoints& endpoints, const StreamTable& table);

std::string depth_target(const VenueEndpoints& endpoints, std::string_view upper_symbol);
std::string ticker_target(const VenueEndpoints& endpoints, std::string_view upper_symbol);

// Every network timing in one place, so tests can shrink them. Defaults are the design values.
struct NetTuning {
    std::size_t limiter_attempts = 200;
    std::chrono::seconds limiter_window = std::chrono::minutes(5);
    std::chrono::milliseconds backoff_initial{500};
    std::chrono::milliseconds backoff_cap = std::chrono::seconds(30);
    double backoff_jitter = 0.2;
    std::chrono::seconds backoff_reset_after{60};
    std::chrono::milliseconds rest_timeout = std::chrono::seconds(10);
    std::chrono::milliseconds validation_give_up = std::chrono::seconds(30);
    std::chrono::milliseconds default_retry_after = std::chrono::seconds(60);
    std::chrono::milliseconds ws_handshake_timeout = std::chrono::seconds(10);
    std::chrono::milliseconds ws_idle_timeout = std::chrono::seconds(30);
    std::chrono::milliseconds close_timeout = std::chrono::seconds(2);
    std::chrono::milliseconds shutdown_deadline = std::chrono::seconds(3);
    std::chrono::milliseconds poll_interval{10};
    std::chrono::milliseconds ring_retry{10};
};

}  // namespace blc
