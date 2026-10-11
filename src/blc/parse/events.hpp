#pragma once

#include "blc/core/types.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace blc {

// Decoded `depthUpdate` from <symbol>@depth@100ms. Levels are absolute quantities in payload
// order; qty 0 means "remove this price".
struct DepthDiff {
    std::uint64_t first_id = 0;      // U
    std::uint64_t last_id = 0;       // u
    std::uint64_t prev_last_id = 0;  // pu (usdm only)
    bool has_prev = false;
    std::vector<Level> bids;
    std::vector<Level> asks;
};

// Decoded <symbol>@depth5@100ms: the exact top 5 per side at update id `id`, best first.
struct Depth5 {
    std::uint64_t id = 0;  // spot: lastUpdateId; usdm: u
    std::array<Level, 5> bids{};
    std::array<Level, 5> asks{};
    std::uint8_t n_bids = 0;
    std::uint8_t n_asks = 0;
};

struct Trade {
    bool buyer_is_maker = false;  // "m"
};

// Decoded REST depth snapshot, best first per side.
struct DepthSnapshot {
    std::uint64_t last_update_id = 0;
    std::vector<Level> bids;
    std::vector<Level> asks;
};

}  // namespace blc
