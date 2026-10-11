#pragma once

#include "blc/book/diff_window.hpp"
#include "blc/book/order_book.hpp"
#include "blc/parse/events.hpp"

#include <cstddef>

namespace blc {

enum class CheckResult { Verified, PartiallyVerified, Mismatch };

// Full check (design §5.6): on each side the book's best levels equal the depth5 levels (same
// count, prices and quantities). `levels_compared` counts the level pairs actually compared,
// including the one that mismatched.
CheckResult full_check(const OrderBook& book, const Depth5& d5,
                       std::size_t& levels_compared) noexcept;

// Partial check against the book state *before* applying `d`: compares only prices inside both
// depth5's range and the book's trusted coverage, and not touched by `d`. Prices outside coverage
// are counted in `levels_unchecked`; a check that compared nothing reports levels_compared == 0.
CheckResult partial_check(const OrderBook& book, const Depth5& d5, const DiffView& d,
                          std::size_t& levels_unchecked, std::size_t& levels_compared) noexcept;

}  // namespace blc
