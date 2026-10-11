#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace blc {

enum class Venue : std::uint8_t { Spot, Usdm };

enum class StreamKind : std::uint8_t { DepthDiff, Depth5, Trade };

constexpr std::string_view to_string(Venue v) noexcept {
    return v == Venue::Spot ? std::string_view{"spot"} : std::string_view{"usdm"};
}

constexpr std::string_view to_string(StreamKind k) noexcept {
    switch (k) {
        case StreamKind::DepthDiff:
            return "depth_diff";
        case StreamKind::Depth5:
            return "depth5";
        case StreamKind::Trade:
            return "trade";
    }
    return "";
}

constexpr std::optional<Venue> parse_venue(std::string_view s) noexcept {
    if (s == "spot") {
        return Venue::Spot;
    }
    if (s == "usdm") {
        return Venue::Usdm;
    }
    return std::nullopt;
}

// Prices and quantities are fixed-point integers with 8 decimals (design §4.2).
inline constexpr std::int64_t kPriceScale = 100'000'000;
inline constexpr std::int64_t kQtyScale = 100'000'000;

struct Level {
    std::int64_t price;
    std::int64_t qty;
};

constexpr bool operator==(const Level& a, const Level& b) noexcept {
    return a.price == b.price && a.qty == b.qty;
}
constexpr bool operator!=(const Level& a, const Level& b) noexcept {
    return !(a == b);
}

// Non-owning view of contiguous levels; never built by allocating.
struct LevelSpan {
    const Level* data = nullptr;
    std::size_t size = 0;
};

inline LevelSpan span_of(const std::vector<Level>& v) noexcept {
    return {v.data(), v.size()};
}

// Bytes that may be parsed in place: `capacity - size` readable padding bytes follow the data.
struct PaddedView {
    const char* data;
    std::size_t size;
    std::size_t capacity;
};

}  // namespace blc
