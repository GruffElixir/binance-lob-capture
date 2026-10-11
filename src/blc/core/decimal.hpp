#pragma once

#include <cstdint>
#include <string_view>

namespace blc {

enum class DecimalError : std::uint8_t { Ok, Empty, InvalidChar, TooPrecise, Overflow };

// Parses a Binance decimal string (`[0-9]+(\.[0-9]+)?`) into an int64 scaled by 10^8.
// Exact: no floating point, no rounding. Digits beyond the 8th decimal must be '0'.
// On any error `out` is left unchanged.
DecimalError parse_e8(std::string_view text, std::int64_t& out) noexcept;

}  // namespace blc
