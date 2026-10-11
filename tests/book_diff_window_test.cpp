#include "blc/book/diff_window.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

DepthDiff make(std::uint64_t U, std::uint64_t u, int n_bids = 1, int n_asks = 0,
               std::uint64_t pu = 0, std::int64_t seed = 1) {
    DepthDiff d;
    d.first_id = U;
    d.last_id = u;
    d.prev_last_id = pu;
    d.has_prev = pu != 0;
    for (int i = 0; i < n_bids; ++i) {
        d.bids.push_back(Level{seed * 1000 + i + 1, i + 1});
    }
    for (int i = 0; i < n_asks; ++i) {
        d.asks.push_back(Level{seed * 2000 + i + 1, i + 2});
    }
    return d;
}

std::vector<Level> to_vector(LevelSpan s) {
    return {s.data, s.data + s.size};
}

TEST(DiffWindow, ViewOf) {
    const auto d = make(1, 2, 2, 1, 7);
    const DiffView v = view_of(d);
    EXPECT_EQ(v.U, 1u);
    EXPECT_EQ(v.u, 2u);
    EXPECT_EQ(v.pu, 7u);
    EXPECT_EQ(to_vector(v.bids), d.bids);
    EXPECT_EQ(to_vector(v.asks), d.asks);
}

TEST(DiffWindow, OverlapAppendedNotReset) {
    DiffWindow w;
    EXPECT_EQ(w.append(Venue::Spot, make(100, 105)), DiffWindow::Append::Appended);
    EXPECT_EQ(w.append(Venue::Spot, make(103, 108)), DiffWindow::Append::Appended);
    EXPECT_EQ(w.size(), 2u);
}

TEST(DiffWindow, GapResets) {
    DiffWindow w;
    w.append(Venue::Spot, make(100, 105));
    w.append(Venue::Spot, make(106, 108));
    EXPECT_EQ(w.append(Venue::Spot, make(110, 112)), DiffWindow::Append::Reset);
    ASSERT_EQ(w.size(), 1u);
    EXPECT_EQ(w.at(0).U, 110u);
}

TEST(DiffWindow, StaleLeavesWindowUnchanged) {
    DiffWindow w;
    w.append(Venue::Spot, make(100, 105, 2, 1));
    EXPECT_EQ(w.append(Venue::Spot, make(101, 105)), DiffWindow::Append::Stale);
    EXPECT_EQ(w.append(Venue::Spot, make(90, 99)), DiffWindow::Append::Stale);
    ASSERT_EQ(w.size(), 1u);
    EXPECT_EQ(w.at(0).bids.size, 2u);

    DiffWindow u;
    u.append(Venue::Usdm, make(100, 105, 1, 0, 99));
    EXPECT_EQ(u.append(Venue::Usdm, make(100, 105, 1, 0, 3)), DiffWindow::Append::Stale);
    EXPECT_EQ(u.append(Venue::Usdm, make(106, 110, 1, 0, 105)), DiffWindow::Append::Appended);
    EXPECT_EQ(u.append(Venue::Usdm, make(111, 115, 1, 0, 109)), DiffWindow::Append::Reset);
    EXPECT_EQ(u.size(), 1u);
}

TEST(DiffWindow, TrimsOldestByLevels) {
    DiffWindow w;
    w.append(Venue::Spot, make(1, 1, 9000, 0, 0, 1));
    w.append(Venue::Spot, make(2, 2, 9000, 0, 0, 2));
    w.append(Venue::Spot, make(3, 3, 9000, 0, 0, 3));
    EXPECT_EQ(w.trimmed(), 1u);
    ASSERT_EQ(w.size(), 2u);
    EXPECT_EQ(w.at(0).U, 2u);
    EXPECT_EQ(w.at(1).U, 3u);
}

TEST(DiffWindow, TrimsOldestByCount) {
    DiffWindow w;
    for (std::uint64_t i = 1; i <= 2001; ++i) {
        w.append(Venue::Spot, make(i, i));
    }
    EXPECT_EQ(w.size(), 2000u);
    EXPECT_EQ(w.at(0).U, 2u);
    EXPECT_EQ(w.at(1999).U, 2001u);
    EXPECT_EQ(w.trimmed(), 1u);
}

TEST(DiffWindow, WrapAroundPreservesContent) {
    DiffWindow w(100, 10);
    std::vector<DepthDiff> all;
    for (std::uint64_t i = 1; i <= 50; ++i) {
        all.push_back(make(i, i, static_cast<int>(i % 7) + 1, static_cast<int>(i % 3), 0,
                           static_cast<std::int64_t>(i)));
        w.append(Venue::Spot, all.back());
    }
    ASSERT_GT(w.size(), 0u);
    for (std::size_t k = 0; k < w.size(); ++k) {
        const auto& expected = all[all.size() - w.size() + k];
        const DiffView v = w.at(k);
        EXPECT_EQ(v.U, expected.first_id);
        EXPECT_EQ(to_vector(v.bids), expected.bids);
        EXPECT_EQ(to_vector(v.asks), expected.asks);
    }
}

TEST(DiffWindow, ArenaKeepsDiffContiguous) {
    DiffWindow w(10, 10);
    w.append(Venue::Spot, make(1, 1, 4, 0, 0, 1));
    w.append(Venue::Spot, make(2, 2, 4, 0, 0, 2));
    w.append(Venue::Spot, make(3, 3, 3, 2, 0, 3));  // does not fit at the end: oldest dropped
    ASSERT_EQ(w.size(), 2u);
    const DiffView v = w.at(1);
    EXPECT_EQ(v.bids.size, 3u);
    EXPECT_EQ(v.asks.size, 2u);
    EXPECT_EQ(v.asks.data, v.bids.data + 3);  // bids and asks stored back to back
    EXPECT_EQ(to_vector(v.bids), make(3, 3, 3, 2, 0, 3).bids);
}

TEST(DiffWindow, ClearAndRestart) {
    DiffWindow w;
    w.append(Venue::Spot, make(1, 2));
    w.restart_with(make(50, 60));
    ASSERT_EQ(w.size(), 1u);
    EXPECT_EQ(w.at(0).U, 50u);
    w.clear();
    EXPECT_EQ(w.size(), 0u);
}

}  // namespace
}  // namespace blc
