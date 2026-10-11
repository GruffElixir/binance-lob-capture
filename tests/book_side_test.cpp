#include "blc/book/book_side.hpp"

#include <map>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

template <Side S>
std::vector<Level> best_levels(const BookSide<S>& side) {
    std::vector<Level> out;
    for (std::size_t i = 0; i < side.size(); ++i) {
        out.push_back(side.best(i));
    }
    return out;
}

TEST(BookSide, BidInsertOverwriteEraseEverywhere) {
    BookSide<Side::Bid> b;
    for (const std::int64_t p : {100, 300, 200, 50, 400}) {
        b.set(p, p / 10);
    }
    EXPECT_EQ(best_levels(b),
              (std::vector<Level>{{400, 40}, {300, 30}, {200, 20}, {100, 10}, {50, 5}}));
    b.set(400, 1);  // overwrite top
    b.set(200, 2);  // overwrite middle
    b.set(50, 3);   // overwrite bottom
    EXPECT_EQ(best_levels(b),
              (std::vector<Level>{{400, 1}, {300, 30}, {200, 2}, {100, 10}, {50, 3}}));
    b.set(400, 0);  // erase top
    b.set(200, 0);  // erase middle
    b.set(50, 0);   // erase bottom
    b.set(999, 0);  // erase absent: no-op
    EXPECT_EQ(best_levels(b), (std::vector<Level>{{300, 30}, {100, 10}}));
}

TEST(BookSide, AskOrderingIsLowestFirst) {
    BookSide<Side::Ask> a;
    for (const std::int64_t p : {105, 101, 103, 102, 104}) {
        a.set(p, 1);
    }
    ASSERT_EQ(a.size(), 5u);
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(a.best(i).price, 101 + static_cast<std::int64_t>(i));
    }
    EXPECT_EQ(a.worst_price(), 105);
    EXPECT_TRUE(BookSide<Side::Ask>::better(101, 102));
    EXPECT_TRUE(BookSide<Side::Bid>::better(102, 101));
}

TEST(BookSide, QtyAtAndClear) {
    BookSide<Side::Bid> b;
    b.set(10, 7);
    EXPECT_EQ(b.qty_at(10), 7);
    EXPECT_FALSE(b.qty_at(11).has_value());
    b.clear();
    EXPECT_EQ(b.size(), 0u);
    EXPECT_FALSE(b.qty_at(10).has_value());
}

TEST(BookSide, LoadBestFirst) {
    BookSide<Side::Bid> b;
    b.load_best_first({{300, 1}, {200, 2}, {100, 3}});
    EXPECT_EQ(best_levels(b), (std::vector<Level>{{300, 1}, {200, 2}, {100, 3}}));
    BookSide<Side::Ask> a;
    a.load_best_first({{100, 1}, {200, 2}});
    EXPECT_EQ(a.best(0).price, 100);
    EXPECT_EQ(a.worst_price(), 200);
}

TEST(BookSide, CountAtOrBetter) {
    BookSide<Side::Bid> b;
    b.load_best_first({{10, 1}, {9, 1}, {8, 1}, {7, 1}, {6, 1}, {5, 1}});
    EXPECT_EQ(b.count_at_or_better(6), 5u);
    EXPECT_EQ(b.count_at_or_better(11), 0u);
    EXPECT_EQ(b.count_at_or_better(1), 6u);
    BookSide<Side::Ask> a;
    a.load_best_first({{1, 1}, {2, 1}, {3, 1}});
    EXPECT_EQ(a.count_at_or_better(2), 2u);
}

TEST(BookSide, TrimKeepsTheBest) {
    BookSide<Side::Bid> b;
    for (std::int64_t p = 1; p <= 5001; ++p) {
        b.set(p, 1);
    }
    EXPECT_FALSE(b.trim_if_over(5001, 4000));
    EXPECT_TRUE(b.trim_if_over(5000, 4000));
    ASSERT_EQ(b.size(), 4000u);
    EXPECT_EQ(b.best(0).price, 5001);
    EXPECT_EQ(b.worst_price(), 1002);
}

TEST(BookSide, MatchesMapUnderRandomUpdatesWithoutReallocating) {
    BookSide<Side::Ask> a;
    const auto cap = a.capacity();
    EXPECT_GE(cap, 5001u);
    std::map<std::int64_t, std::int64_t> ref;
    std::mt19937 rng(7);
    std::uniform_int_distribution<std::int64_t> price(1, 4000);
    std::uniform_int_distribution<int> qty(0, 3);
    for (int i = 0; i < 10'000; ++i) {
        const std::int64_t p = price(rng);
        const std::int64_t q = qty(rng);
        a.set(p, q);
        if (q == 0) {
            ref.erase(p);
        } else {
            ref[p] = q;
        }
        ASSERT_EQ(a.size(), ref.size());
    }
    std::size_t i = 0;
    for (const auto& [p, q] : ref) {  // ascending = best-first for asks
        ASSERT_EQ(a.best(i), (Level{p, q}));
        ++i;
    }
    EXPECT_EQ(a.capacity(), cap);
}

}  // namespace
}  // namespace blc
