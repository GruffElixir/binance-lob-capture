#pragma once

#include "blc/core/types.hpp"
#include "blc/parse/events.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace blc {

inline constexpr std::size_t kWindowMaxLevels = 20'000;
inline constexpr std::size_t kWindowMaxDiffs = 2'000;

// One representation for live (just parsed) and windowed diffs.
struct DiffView {
    std::uint64_t U = 0;
    std::uint64_t u = 0;
    std::uint64_t pu = 0;
    LevelSpan bids;
    LevelSpan asks;
};

DiffView view_of(const DepthDiff& diff) noexcept;

// The most recent contiguous diffs of one symbol, decoded into owned integers (design §5.4).
// Storage is reserved once: a level arena and a circular array of diff headers. A diff that
// does not fit is admitted by dropping the oldest diffs; each diff's levels stay contiguous.
class DiffWindow {
public:
    enum class Append { Appended, Stale, Reset };

    explicit DiffWindow(std::size_t max_levels = kWindowMaxLevels,
                        std::size_t max_diffs = kWindowMaxDiffs);

    // Classifies `diff` against the last stored u: stale -> not stored; gap -> the window
    // restarts with it; continues -> appended.
    Append append(Venue venue, const DepthDiff& diff) noexcept;
    void restart_with(const DepthDiff& diff) noexcept;
    void clear() noexcept;

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }
    DiffView at(std::size_t i) const noexcept;  // 0 is the oldest
    std::uint64_t trimmed() const noexcept { return trimmed_; }

private:
    struct Header {
        std::uint64_t U;
        std::uint64_t u;
        std::uint64_t pu;
        std::size_t offset;
        std::uint32_t n_bids;
        std::uint32_t n_asks;
    };

    void store(const DepthDiff& diff) noexcept;
    void drop_oldest() noexcept;
    const Header& header(std::size_t i) const noexcept {
        return headers_[(first_ + i) % headers_.size()];
    }

    std::vector<Level> levels_;
    std::vector<Header> headers_;
    std::size_t first_ = 0;
    std::size_t count_ = 0;
    std::size_t begin_ = 0;  // arena offset of the oldest diff's levels
    std::size_t end_ = 0;    // arena offset one past the newest diff's levels
    std::uint64_t trimmed_ = 0;
};

}  // namespace blc
