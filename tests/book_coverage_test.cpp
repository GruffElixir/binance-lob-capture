#include "blc/book/order_book.hpp"

#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

// Snapshot side with prices from `best` stepping by `step` (best first), qty 1.
std::vector<Level> ladder(std::int64_t best, std::int64_t step, int n) {
    std::vector<Level> v;
    for (int i = 0; i < n; ++i) {
        v.push_back(Level{best + step * i, 1});
    }
    return v;
}

DepthSnapshot snapshot(std::vector<Level> bids, std::vector<Level> asks, std::uint64_t id = 1) {
    DepthSnapshot s;
    s.last_update_id = id;
    s.bids = std::move(bids);
    s.asks = std::move(asks);
    return s;
}

void apply_bids(OrderBook& book, const std::vector<Level>& bids) {
    book.apply(span_of(bids), LevelSpan{});
}

TEST(Coverage, ShortSnapshotIsComplete) {
    OrderBook book;
    book.load(snapshot({{30, 1}, {20, 2}, {10, 3}}, {{40, 4}}));
    EXPECT_TRUE(book.coverage(Side::Bid).complete);
    EXPECT_EQ(book.margin(Side::Bid), std::numeric_limits<std::size_t>::max());
    EXPECT_TRUE(book.authoritative());
    TopOfBook top;
    book.top5(top);
    EXPECT_EQ(top.bids[0], (Level{30, 1}));
    EXPECT_EQ(top.bids[2], (Level{10, 3}));
    EXPECT_EQ(top.bids[3], (Level{0, 0}));
    EXPECT_EQ(top.asks[0], (Level{40, 4}));
    EXPECT_EQ(top.asks[1], (Level{0, 0}));
}

TEST(Coverage, FullSnapshotBounded) {
    OrderBook book;
    book.load(snapshot(ladder(1000, -1, 1000), ladder(2000, 1, 1000)));
    EXPECT_FALSE(book.coverage(Side::Bid).complete);
    EXPECT_EQ(book.coverage(Side::Bid).boundary, 1);
    EXPECT_EQ(book.margin(Side::Bid), 1000u);
    EXPECT_EQ(book.coverage(Side::Ask).boundary, 2999);
    EXPECT_EQ(book.margin(Side::Ask), 1000u);
    EXPECT_TRUE(book.authoritative());
}

TEST(Coverage, ExhaustionAfterDeletes) {
    OrderBook book;
    book.load(snapshot(ladder(2000, -1, 1000), ladder(3000, 1, 1000)));
    EXPECT_EQ(book.coverage(Side::Bid).boundary, 1001);
    std::vector<Level> deletes;
    for (std::int64_t p = 2000; p > 1004; --p) {  // the 996 best
        deletes.push_back(Level{p, 0});
    }
    apply_bids(book, deletes);
    EXPECT_EQ(book.margin(Side::Bid), 4u);
    EXPECT_FALSE(book.authoritative());
    apply_bids(book, {{1000, 5}});  // below the boundary: does not count
    EXPECT_EQ(book.margin(Side::Bid), 4u);
    EXPECT_FALSE(book.authoritative());
}

TEST(Coverage, TrimCompleteSideRevokesCompleteness) {
    OrderBook book;
    book.load(snapshot({{10'000, 1}}, {{20'000, 1}}));
    ASSERT_TRUE(book.coverage(Side::Bid).complete);
    apply_bids(book, ladder(9999, -1, 5000));  // side grows to 5001 -> trimmed to 4000
    EXPECT_EQ(book.bids().size(), 4000u);
    EXPECT_FALSE(book.coverage(Side::Bid).complete);
    EXPECT_EQ(book.coverage(Side::Bid).boundary, book.bids().worst_price());
    EXPECT_EQ(book.margin(Side::Bid), 4000u);
    std::vector<Level> deletes;
    for (std::size_t i = 0; i < 3996; ++i) {
        deletes.push_back(Level{book.bids().best(i).price, 0});
    }
    apply_bids(book, deletes);
    EXPECT_EQ(book.bids().size(), 4u);
    EXPECT_FALSE(book.authoritative());  // no zero padding: evicted levels still exist
}

TEST(Coverage, TrimBeyondBoundaryKeepsBoundary) {
    OrderBook book;
    book.load(snapshot(ladder(10'000, -1, 1000), ladder(20'000, 1, 1000)));
    ASSERT_EQ(book.coverage(Side::Bid).boundary, 9001);
    apply_bids(book, ladder(4001, -1, 4001));  // 4001 levels, all worse than the boundary
    EXPECT_EQ(book.bids().size(), 4000u);
    EXPECT_EQ(book.coverage(Side::Bid).boundary, 9001);
    EXPECT_EQ(book.margin(Side::Bid), 1000u);
}

TEST(Coverage, TrimInsideBoundaryMovesBoundary) {
    OrderBook book;
    book.load(snapshot(ladder(5000, -1, 1000), ladder(20'000, 1, 1000)));
    ASSERT_EQ(book.coverage(Side::Bid).boundary, 4001);
    apply_bids(book, ladder(9001, -1, 4001));  // 5001 trusted levels -> trimmed to 4000
    EXPECT_EQ(book.bids().size(), 4000u);
    EXPECT_EQ(book.coverage(Side::Bid).boundary, 5002);
    EXPECT_EQ(book.margin(Side::Bid), 4000u);

    OrderBook asks;
    asks.load(snapshot(ladder(100, -1, 10), ladder(5000, 1, 1000)));  // asks 5000..5999
    ASSERT_EQ(asks.coverage(Side::Ask).boundary, 5999);
    asks.apply(LevelSpan{}, span_of(ladder(999, 1, 4001)));  // 999..4999, all better
    EXPECT_EQ(asks.asks().size(), 4000u);
    EXPECT_EQ(asks.coverage(Side::Ask).boundary, 4998);
    EXPECT_EQ(asks.margin(Side::Ask), 4000u);
}

TEST(Coverage, ClearResets) {
    OrderBook book;
    book.load(snapshot({{1, 1}}, {{2, 1}}, 7));
    book.id = 7;
    book.clear();
    EXPECT_EQ(book.bids().size(), 0u);
    EXPECT_EQ(book.asks().size(), 0u);
}

}  // namespace
}  // namespace blc
