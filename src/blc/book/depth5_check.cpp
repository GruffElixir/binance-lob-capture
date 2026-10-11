#include "blc/book/depth5_check.hpp"

#include <array>

namespace blc {
namespace {

template <Side S>
bool full_side(const BookSide<S>& side, const std::array<Level, 5>& shown, std::size_t n,
               std::size_t& compared) noexcept {
    // A side shown with fewer than 5 levels is the whole exchange side.
    if (n < shown.size() ? side.size() != n : side.size() < n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        ++compared;
        if (side.best(i) != shown[i]) {
            return false;
        }
    }
    return true;
}

bool touched(LevelSpan diff_side, std::int64_t price) noexcept {
    for (std::size_t i = 0; i < diff_side.size; ++i) {
        if (diff_side.data[i].price == price) {
            return true;
        }
    }
    return false;
}

template <Side S>
bool partial_side(const BookSide<S>& side, const Coverage& coverage,
                  const std::array<Level, 5>& shown, std::size_t n, LevelSpan diff_side,
                  std::size_t& unchecked, std::size_t& compared) noexcept {
    const auto at_or_better = [](std::int64_t a, std::int64_t b) {
        return a == b || BookSide<S>::better(a, b);
    };
    // depth5's range: everything at or better than its 5th level, or the whole side if shorter.
    const bool bounded_range = n == shown.size();
    const std::int64_t range_edge = bounded_range ? shown[n - 1].price : 0;
    const auto in_range = [&](std::int64_t p) {
        return !bounded_range || at_or_better(p, range_edge);
    };
    const auto in_coverage = [&](std::int64_t p) {
        return coverage.complete || at_or_better(p, coverage.boundary);
    };

    // Merge book levels (in range) and depth5 levels, best first.
    std::size_t i = 0;  // depth5 index
    std::size_t j = 0;  // book index
    for (;;) {
        const bool have_shown = i < n;
        const bool have_book = j < side.size() && in_range(side.best(j).price);
        if (!have_shown && !have_book) {
            return true;
        }
        std::int64_t price = 0;
        bool shown_has = false;
        bool book_has = false;
        if (have_shown && have_book && shown[i].price == side.best(j).price) {
            price = shown[i].price;
            shown_has = book_has = true;
        } else if (have_shown &&
                   (!have_book || BookSide<S>::better(shown[i].price, side.best(j).price))) {
            price = shown[i].price;
            shown_has = true;
        } else {
            price = side.best(j).price;
            book_has = true;
        }
        const std::int64_t shown_qty = shown_has ? shown[i].qty : 0;
        const std::int64_t book_qty = book_has ? side.best(j).qty : 0;
        if (shown_has) {
            ++i;
        }
        if (book_has) {
            ++j;
        }
        if (touched(diff_side, price)) {
            continue;  // its state at p is unknown: it changed within d's window
        }
        if (!in_coverage(price)) {
            ++unchecked;  // beyond trusted coverage: never compared
            continue;
        }
        ++compared;
        if (shown_has != book_has || shown_qty != book_qty) {
            return false;
        }
    }
}

}  // namespace

CheckResult full_check(const OrderBook& book, const Depth5& d5,
                       std::size_t& levels_compared) noexcept {
    levels_compared = 0;
    if (!full_side(book.bids(), d5.bids, d5.n_bids, levels_compared) ||
        !full_side(book.asks(), d5.asks, d5.n_asks, levels_compared)) {
        return CheckResult::Mismatch;
    }
    return CheckResult::Verified;
}

CheckResult partial_check(const OrderBook& book, const Depth5& d5, const DiffView& d,
                          std::size_t& levels_unchecked, std::size_t& levels_compared) noexcept {
    levels_unchecked = 0;
    levels_compared = 0;
    if (!partial_side(book.bids(), book.coverage(Side::Bid), d5.bids, d5.n_bids, d.bids,
                      levels_unchecked, levels_compared) ||
        !partial_side(book.asks(), book.coverage(Side::Ask), d5.asks, d5.n_asks, d.asks,
                      levels_unchecked, levels_compared)) {
        return CheckResult::Mismatch;
    }
    return CheckResult::PartiallyVerified;
}

}  // namespace blc
