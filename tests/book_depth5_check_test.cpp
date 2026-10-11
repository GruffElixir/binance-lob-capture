#include "blc/book/depth5_check.hpp"

#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

OrderBook book_with(std::vector<Level> bids, std::vector<Level> asks, std::size_t limit = 1000) {
    DepthSnapshot s;
    s.last_update_id = 100;
    s.bids = std::move(bids);
    s.asks = std::move(asks);
    OrderBook b;
    b.load(s, limit);
    return b;
}

Depth5 depth5_of(std::vector<Level> bids, std::vector<Level> asks, std::uint64_t id = 100) {
    Depth5 d{};
    d.id = id;
    for (const auto& l : bids) {
        d.bids[d.n_bids++] = l;
    }
    for (const auto& l : asks) {
        d.asks[d.n_asks++] = l;
    }
    return d;
}

const std::vector<Level> kBids = {{10, 1}, {9, 1}, {8, 1}, {7, 1}, {6, 1}};
const std::vector<Level> kAsks = {{11, 1}, {12, 1}, {13, 1}, {14, 1}, {15, 1}};

TEST(Depth5Check, FullMatchAndMismatch) {
    const auto book = book_with(kBids, kAsks);
    std::size_t compared = 0;
    EXPECT_EQ(full_check(book, depth5_of(kBids, kAsks), compared), CheckResult::Verified);
    EXPECT_EQ(compared, 10u);

    auto other = kBids;
    other[2].qty = 9;
    EXPECT_EQ(full_check(book, depth5_of(other, kAsks), compared), CheckResult::Mismatch);
    EXPECT_EQ(compared, 3u);  // counted up to and including the mismatch

    EXPECT_EQ(full_check(book, depth5_of({kBids.begin(), kBids.begin() + 4}, kAsks), compared),
              CheckResult::Mismatch);  // depth5 shows 4 levels, the book has 5
}

TEST(Depth5Check, FullCheckOnShortCompleteSide) {
    const auto book = book_with({{10, 1}, {9, 2}}, kAsks);
    std::size_t compared = 0;
    EXPECT_EQ(full_check(book, depth5_of({{10, 1}, {9, 2}}, kAsks), compared),
              CheckResult::Verified);
    EXPECT_EQ(compared, 7u);
}

TEST(Depth5Check, PartialIgnoresTouchedPrices) {
    const auto book = book_with(kBids, kAsks);
    auto shown = kBids;
    shown[0].qty = 5;  // price 10 changed inside d's window
    DepthDiff d;
    d.bids = {{10, 7}};
    std::size_t unchecked = 0;
    std::size_t compared = 0;
    EXPECT_EQ(partial_check(book, depth5_of(shown, kAsks), view_of(d), unchecked, compared),
              CheckResult::PartiallyVerified);
    EXPECT_EQ(unchecked, 0u);
    EXPECT_EQ(compared, 9u);
}

TEST(Depth5Check, PartialBeyondCoverageNotCompared) {
    // Spec §5.6: trusted bids 10..6 with boundary 6; d deletes 10 and adds 11; depth5 shows 9..5
    // and 5 was never known.
    const auto book = book_with(kBids, kAsks, 5);
    ASSERT_FALSE(book.coverage(Side::Bid).complete);
    ASSERT_EQ(book.coverage(Side::Bid).boundary, 6);
    DepthDiff d;
    d.bids = {{10, 0}, {11, 3}};
    std::size_t unchecked = 0;
    std::size_t compared = 0;
    EXPECT_EQ(partial_check(book, depth5_of({{9, 1}, {8, 1}, {7, 1}, {6, 1}, {5, 4}}, kAsks),
                            view_of(d), unchecked, compared),
              CheckResult::PartiallyVerified);
    EXPECT_EQ(unchecked, 1u);
    EXPECT_EQ(compared, 9u);  // 9,8,7,6 + five asks
}

TEST(Depth5Check, PartialDetectsUntouchedDifference) {
    const auto book = book_with(kBids, kAsks);
    auto shown = kBids;
    shown[2].qty = 3;  // price 8, untouched by d
    DepthDiff d;
    d.bids = {{10, 2}};
    std::size_t unchecked = 0;
    std::size_t compared = 0;
    EXPECT_EQ(partial_check(book, depth5_of(shown, kAsks), view_of(d), unchecked, compared),
              CheckResult::Mismatch);
    EXPECT_GE(compared, 1u);
}

TEST(Depth5Check, PartialDetectsMissingUntouchedLevel) {
    const auto book = book_with(kBids, kAsks);
    std::vector<Level> shown = {{10, 1}, {9, 1}, {7, 1}, {6, 1}, {5, 1}};  // 8 missing, untouched
    DepthDiff d;
    std::size_t unchecked = 0;
    std::size_t compared = 0;
    EXPECT_EQ(partial_check(book, depth5_of(shown, kAsks), view_of(d), unchecked, compared),
              CheckResult::Mismatch);
}

TEST(Depth5Check, ComparedCountsExcludeUncheckedAndPadding) {
    const auto book = book_with(kBids, kAsks, 5);
    DepthDiff all_touched;
    for (const auto& l : kBids) {
        all_touched.bids.push_back(l);
    }
    for (const auto& l : kAsks) {
        all_touched.asks.push_back(l);
    }
    std::size_t unchecked = 0;
    std::size_t compared = 0;
    EXPECT_EQ(
        partial_check(book, depth5_of(kBids, kAsks), view_of(all_touched), unchecked, compared),
        CheckResult::PartiallyVerified);
    EXPECT_EQ(compared, 0u);  // nothing comparable: not evidence of validation

    const auto short_book = book_with({{10, 1}}, {{11, 1}});
    EXPECT_EQ(full_check(short_book, depth5_of({{10, 1}}, {{11, 1}}), compared),
              CheckResult::Verified);
    EXPECT_EQ(compared, 2u);  // padding is never counted
}

}  // namespace
}  // namespace blc
