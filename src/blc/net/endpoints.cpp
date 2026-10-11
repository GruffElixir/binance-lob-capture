#include "blc/net/endpoints.hpp"

#include "blc/core/errors.hpp"
#include "blc/parse/payload_parser.hpp"

namespace blc {

VenueEndpoints default_endpoints(Venue venue) {
    if (venue == Venue::Spot) {
        return VenueEndpoints{Endpoint{"stream.binance.com", "9443", "/stream?streams="},
                              Endpoint{"api.binance.com", "443", ""}, "/api/v3/depth",
                              "/api/v3/ticker/price"};
    }
    return VenueEndpoints{Endpoint{"fstream.binance.com", "443", "/public/stream?streams="},
                          Endpoint{"fapi.binance.com", "443", ""}, "/fapi/v1/depth",
                          "/fapi/v1/ticker/price"};
}

std::vector<std::vector<std::uint16_t>> make_shards(std::size_t n_symbols, int per_connection) {
    const std::size_t per = per_connection > 0 ? static_cast<std::size_t>(per_connection) : 1;
    std::vector<std::vector<std::uint16_t>> shards;
    for (std::size_t i = 0; i < n_symbols; ++i) {
        if (i % per == 0) {
            shards.emplace_back();
        }
        shards.back().push_back(static_cast<std::uint16_t>(i));
    }
    return shards;
}

std::string ws_target(const VenueEndpoints& endpoints, const StreamTable& table) {
    std::string target = endpoints.ws.target_prefix;
    table.append_stream_list(target);
    if (target.size() > kMaxTargetBytes) {
        throw UsageError("the WebSocket URL for one connection would be " +
                         std::to_string(target.size()) + " bytes (limit " +
                         std::to_string(kMaxTargetBytes) + "); lower --symbols-per-connection");
    }
    return target;
}

std::string depth_target(const VenueEndpoints& endpoints, std::string_view upper_symbol) {
    return endpoints.depth_path + "?symbol=" + std::string(upper_symbol) +
           "&limit=" + std::to_string(kMaxSnapshotLevelsPerSide);
}

std::string ticker_target(const VenueEndpoints& endpoints, std::string_view upper_symbol) {
    return endpoints.ticker_path + "?symbol=" + std::string(upper_symbol);
}

}  // namespace blc
