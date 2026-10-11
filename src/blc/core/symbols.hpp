#pragma once

#include "blc/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace blc {

inline constexpr std::size_t kMaxSymbols = 256;

struct SymbolSpec {
    std::string upper;  // as written to CSVs, e.g. "BTCUSDT"
    std::string lower;  // as used in stream names, e.g. "btcusdt"
    std::int32_t id;    // instrument_id(venue, upper)
};

// Parses a --symbols list (design §2): comma-separated, case-insensitive, items trimmed,
// each 2-20 characters of A-Z0-9, no duplicates, 1-256 items, distinct instrument ids.
// Throws UsageError naming the offending item.
std::vector<SymbolSpec> parse_symbol_list(Venue venue, std::string_view list);

}  // namespace blc
