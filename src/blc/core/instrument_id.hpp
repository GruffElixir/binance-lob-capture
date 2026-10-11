#pragma once

#include "blc/core/types.hpp"

#include <cstdint>
#include <string_view>

namespace blc {

constexpr std::uint32_t fnv1a32(std::string_view bytes) noexcept {
    std::uint32_t h = 2166136261u;
    for (const char c : bytes) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 16777619u;
    }
    return h;
}

// Stable id for a (venue, symbol) pair: FNV-1a-32 of "<venue>:<SYMBOL>" masked to 31 bits
// (design §4.3). Independent of argument order, so capture and replay agree.
inline std::int32_t instrument_id(Venue venue, std::string_view upper_symbol) noexcept {
    std::uint32_t h = 2166136261u;
    const auto mix = [&h](std::string_view part) {
        for (const char c : part) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 16777619u;
        }
    };
    mix(to_string(venue));
    mix(":");
    mix(upper_symbol);
    return static_cast<std::int32_t>(h & 0x7FFFFFFFu);
}

}  // namespace blc
