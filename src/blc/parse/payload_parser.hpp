#pragma once

#include "blc/core/types.hpp"
#include "blc/parse/events.hpp"

#include <cstddef>
#include <cstdint>
#include <simdjson.h>
#include <string_view>

namespace blc {

enum class ParseStatus : std::uint8_t { Ok, Malformed, BadDecimal, SymbolMismatch, Oversized };

inline constexpr std::size_t kMaxDiffLevels = 20'000;
inline constexpr std::size_t kMaxSnapshotLevelsPerSide = 1'000;
inline constexpr std::size_t kMaxPayloadBytes = std::size_t{1} << 20;

// Decodes Binance payloads into typed events in one forward pass (design §6.5): every field is
// visited (unknown values validated, never skipped), required keys appear exactly once, the
// document must be fully consumed, and decoded sizes are capped while decoding so the reusable
// event storage never grows. Never allocates after construction.
class PayloadParser {
public:
    PayloadParser();  // allocates the parser for kMaxPayloadBytes

    ParseStatus diff(Venue venue, PaddedView payload, std::string_view upper_symbol,
                     DepthDiff& out) noexcept;
    ParseStatus depth5(Venue venue, PaddedView payload, std::string_view upper_symbol,
                       Depth5& out) noexcept;
    ParseStatus trade(Venue venue, PaddedView payload, std::string_view upper_symbol,
                      Trade& out) noexcept;
    ParseStatus snapshot(Venue venue, PaddedView payload, DepthSnapshot& out) noexcept;

    // Reusable events with storage reserved to the caps (one diff side may hold every level).
    static DepthDiff make_diff();
    static DepthSnapshot make_snapshot();

    std::size_t retained_bytes() const noexcept { return parser_.capacity(); }

private:
    simdjson::ondemand::parser parser_;
};

}  // namespace blc
