#pragma once

#include "blc/core/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace blc {

enum class Side : std::uint8_t { Bid, Ask };

inline constexpr std::size_t kBookSideCap = 5000;
inline constexpr std::size_t kBookSideTrimTo = 4000;

// One side of the book as a sorted flat vector with the BEST price at the back (bids ascending,
// asks descending), design §5.1. Updates near the touch move only the few levels better than
// them; the search gallops from the back. Storage is reserved once and never reallocates while
// the side stays within the cap (+1 insertion).
template <Side S>
class BookSide {
public:
    explicit BookSide(std::size_t reserve = kBookSideCap + 1) { levels_.reserve(reserve); }

    // True when price `a` is strictly better than `b` on this side.
    static constexpr bool better(std::int64_t a, std::int64_t b) noexcept {
        return S == Side::Bid ? a > b : a < b;
    }

    // Absolute quantity update; qty 0 erases (erasing an absent level is a no-op).
    void set(std::int64_t price, std::int64_t qty) noexcept {
        const std::size_t pos = position(price);
        const bool found = pos < levels_.size() && levels_[pos].price == price;
        const auto it = levels_.begin() + static_cast<std::ptrdiff_t>(pos);
        if (qty == 0) {
            if (found) {
                levels_.erase(it);
            }
        } else if (found) {
            levels_[pos].qty = qty;
        } else {
            levels_.insert(it, Level{price, qty});
        }
    }

    std::size_t size() const noexcept { return levels_.size(); }
    bool empty() const noexcept { return levels_.empty(); }
    std::size_t capacity() const noexcept { return levels_.capacity(); }

    // i-th best level, i < size().
    const Level& best(std::size_t i) const noexcept { return levels_[levels_.size() - 1 - i]; }

    std::optional<std::int64_t> qty_at(std::int64_t price) const noexcept {
        const std::size_t pos = position(price);
        if (pos < levels_.size() && levels_[pos].price == price) {
            return levels_[pos].qty;
        }
        return std::nullopt;
    }

    void clear() noexcept { levels_.clear(); }

    // Replaces the side with `best_first` (best level first, as Binance sends it).
    void load_best_first(const std::vector<Level>& best_first) noexcept {
        levels_.assign(best_first.rbegin(), best_first.rend());
    }

    // Number of levels priced at or better than `boundary`.
    std::size_t count_at_or_better(std::int64_t boundary) const noexcept {
        return levels_.size() - position(boundary);
    }

    // Worst (farthest from the touch) price; requires !empty().
    std::int64_t worst_price() const noexcept { return levels_.front().price; }

    // If the side holds more than `cap` levels, drops the worst ones down to `to`.
    bool trim_if_over(std::size_t cap = kBookSideCap, std::size_t to = kBookSideTrimTo) noexcept {
        if (levels_.size() <= cap) {
            return false;
        }
        levels_.erase(levels_.begin(),
                      levels_.begin() + static_cast<std::ptrdiff_t>(levels_.size() - to));
        return true;
    }

private:
    // Storage order: "worse before better".
    static constexpr bool stored_before(std::int64_t a, std::int64_t b) noexcept {
        return better(b, a);
    }

    // First index whose price is not stored before `price` (lower bound), found by galloping
    // from the back (the best end), so near-touch lookups cost O(log k).
    std::size_t position(std::int64_t price) const noexcept {
        const std::size_t n = levels_.size();
        std::size_t hi = n;
        std::size_t lo = 0;
        std::size_t step = 1;
        while (step <= n) {
            const std::size_t idx = n - step;
            if (stored_before(levels_[idx].price, price)) {
                lo = idx + 1;
                break;
            }
            hi = idx;
            step *= 2;
        }
        const auto first = levels_.begin() + static_cast<std::ptrdiff_t>(lo);
        const auto last = levels_.begin() + static_cast<std::ptrdiff_t>(hi);
        const auto it = std::lower_bound(first, last, price, [](const Level& l, std::int64_t p) {
            return stored_before(l.price, p);
        });
        return static_cast<std::size_t>(it - levels_.begin());
    }

    std::vector<Level> levels_;
};

}  // namespace blc
