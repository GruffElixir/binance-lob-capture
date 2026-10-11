#pragma once

#include "blc/book/book_side.hpp"
#include "blc/core/types.hpp"
#include "blc/parse/events.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace blc {

inline constexpr std::size_t kSnapshotLimit = 1000;
inline constexpr std::size_t kMinAuthoritativeLevels = 5;

// How far a side is known to be complete (design §5.3).
struct Coverage {
    bool complete = true;       // the snapshot held the entire side
    std::int64_t boundary = 0;  // when !complete: every price at or better than this is known
};

struct TopOfBook {
    std::array<Level, 5> bids{};  // best first; missing levels are {0, 0}
    std::array<Level, 5> asks{};
};

class OrderBook {
public:
    // Replaces the book with a REST snapshot. A side with fewer than `limit` levels is complete;
    // otherwise its boundary is the snapshot's worst price on that side.
    void load(const DepthSnapshot& snapshot, std::size_t limit = kSnapshotLimit) noexcept;

    // Applies absolute level quantities (qty 0 erases). Trims after every insertion that takes a
    // side over the cap, adjusting coverage (a complete side becomes bounded).
    void apply(LevelSpan bids, LevelSpan asks) noexcept;

    // Levels at or better than the boundary; SIZE_MAX for a complete side.
    std::size_t margin(Side side) const noexcept;

    // Both sides complete or with at least 5 levels inside coverage.
    bool authoritative() const noexcept;

    void top5(TopOfBook& out) const noexcept;
    void clear() noexcept;

    const Coverage& coverage(Side side) const noexcept {
        return side == Side::Bid ? bid_coverage_ : ask_coverage_;
    }
    std::size_t retained_bytes() const noexcept {
        return (bids_.capacity() + asks_.capacity()) * sizeof(Level);
    }
    const BookSide<Side::Bid>& bids() const noexcept { return bids_; }
    const BookSide<Side::Ask>& asks() const noexcept { return asks_; }

    std::uint64_t id{};  // last applied update id

private:
    template <Side S>
    static void apply_side(BookSide<S>& side, Coverage& coverage, LevelSpan levels) noexcept;

    BookSide<Side::Bid> bids_;
    BookSide<Side::Ask> asks_;
    Coverage bid_coverage_;
    Coverage ask_coverage_;
};

}  // namespace blc
