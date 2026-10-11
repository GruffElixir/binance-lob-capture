#include "blc/book/order_book.hpp"

#include <limits>

namespace blc {
namespace {

Coverage coverage_from_snapshot(const std::vector<Level>& side, std::size_t limit) noexcept {
    if (side.size() < limit) {
        return Coverage{true, 0};
    }
    return Coverage{false, side.back().price};
}

template <Side S>
std::size_t side_margin(const BookSide<S>& side, const Coverage& coverage) noexcept {
    if (coverage.complete) {
        return std::numeric_limits<std::size_t>::max();
    }
    return side.count_at_or_better(coverage.boundary);
}

template <Side S>
void top_of(const BookSide<S>& side, std::array<Level, 5>& out) noexcept {
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = i < side.size() ? side.best(i) : Level{0, 0};
    }
}

}  // namespace

void OrderBook::load(const DepthSnapshot& snapshot, std::size_t limit) noexcept {
    bids_.load_best_first(snapshot.bids);
    asks_.load_best_first(snapshot.asks);
    bid_coverage_ = coverage_from_snapshot(snapshot.bids, limit);
    ask_coverage_ = coverage_from_snapshot(snapshot.asks, limit);
    id = snapshot.last_update_id;
}

template <Side S>
void OrderBook::apply_side(BookSide<S>& side, Coverage& coverage, LevelSpan levels) noexcept {
    for (std::size_t i = 0; i < levels.size; ++i) {
        side.set(levels.data[i].price, levels.data[i].qty);
        if (side.trim_if_over()) {
            const std::int64_t worst = side.worst_price();
            if (coverage.complete) {
                // Evicted levels still exist on the exchange: completeness is revoked.
                coverage = Coverage{false, worst};
            } else if (BookSide<S>::better(worst, coverage.boundary)) {
                // The trim removed levels at or better than the boundary.
                coverage.boundary = worst;
            }
        }
    }
}

void OrderBook::apply(LevelSpan bids, LevelSpan asks) noexcept {
    apply_side(bids_, bid_coverage_, bids);
    apply_side(asks_, ask_coverage_, asks);
}

std::size_t OrderBook::margin(Side side) const noexcept {
    return side == Side::Bid ? side_margin(bids_, bid_coverage_)
                             : side_margin(asks_, ask_coverage_);
}

bool OrderBook::authoritative() const noexcept {
    return margin(Side::Bid) >= kMinAuthoritativeLevels &&
           margin(Side::Ask) >= kMinAuthoritativeLevels;
}

void OrderBook::top5(TopOfBook& out) const noexcept {
    top_of(bids_, out.bids);
    top_of(asks_, out.asks);
}

void OrderBook::clear() noexcept {
    bids_.clear();
    asks_.clear();
    bid_coverage_ = Coverage{};
    ask_coverage_ = Coverage{};
    id = 0;
}

}  // namespace blc
