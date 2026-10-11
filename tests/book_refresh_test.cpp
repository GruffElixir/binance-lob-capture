#include "blc/book/symbol_book.hpp"
#include "blc/parse/payload_parser.hpp"

#include <map>

#include <gtest/gtest.h>
namespace blc {
namespace {
DepthSnapshot snapshot(std::uint64_t id, std::int64_t bid = 10000, std::int64_t ask = 20000) {
    auto s = PayloadParser::make_snapshot();
    s.last_update_id = id;
    for (std::int64_t i = 0; i < 1000; ++i) {
        s.bids.push_back({bid - i, 1});
        s.asks.push_back({ask + i, 1});
    }
    return s;
}
DepthDiff diff(std::uint64_t id) {
    auto d = PayloadParser::make_diff();
    d.first_id = d.last_id = id;
    d.prev_last_id = id - 1;
    return d;
}
struct Refresh : ::testing::Test {
    SymbolCounters counters;
    SymbolBook book{Venue::Spot, counters};
    void SetUp() override {
        book.enter_unsynced(UnsyncCause::NewEpoch);
        book.on_diff(diff(101));
        ASSERT_TRUE(book.on_snapshot(snapshot(100)).row);
        auto depletion = diff(102);
        for (std::int64_t i = 0; i < 850; ++i) {
            depletion.bids.push_back({10000 - i, 0});
            depletion.asks.push_back({20000 + i, 0});
        }
        ASSERT_TRUE(book.on_diff(depletion).row);
    }
};
TEST_F(Refresh, RequestsBelowTwoHundredAndAvoidsDuplicates) {
    ASSERT_EQ(book.book().margin(Side::Bid), 150u);
    EXPECT_TRUE(book.snapshot_wanted());
    book.mark_requested();
    EXPECT_FALSE(book.snapshot_wanted());
}
TEST_F(Refresh, EqualShadowSwapsOnSnapshot) {
    book.mark_requested();
    book.on_diff(diff(103));
    const auto e = book.on_snapshot(snapshot(102, 9150, 20850));
    EXPECT_TRUE(e.row);
    EXPECT_EQ(e.type, 'R');
    EXPECT_EQ(counters.refresh_ok, 1u);
    EXPECT_EQ(book.book().margin(Side::Bid), 1000u);
    EXPECT_FALSE(book.snapshot_wanted());
}
TEST_F(Refresh, FutureShadowSwapsOnLaterDiff) {
    book.mark_requested();
    EXPECT_FALSE(book.on_snapshot(snapshot(102, 9150, 20850)).row);
    EXPECT_FALSE(book.snapshot_wanted());
    const auto e = book.on_diff(diff(103));
    EXPECT_TRUE(e.row);
    EXPECT_EQ(e.type, 'D');
    EXPECT_EQ(counters.refresh_ok, 1u);
    EXPECT_EQ(book.book().id, 103u);
}
TEST_F(Refresh, TooOldShadowIsRefetched) {
    book.mark_requested();
    EXPECT_FALSE(book.on_snapshot(snapshot(1)).row);
    EXPECT_TRUE(book.snapshot_wanted());
    EXPECT_EQ(counters.snapshot_too_old, 1u);
    EXPECT_EQ(book.state(), SyncState::Synced);
}
TEST_F(Refresh, DifferentTopReplacesLiveAndCountsMismatch) {
    book.mark_requested();
    book.on_diff(diff(103));
    EXPECT_TRUE(book.on_snapshot(snapshot(102, 9151, 20850)).row);
    EXPECT_EQ(counters.refresh_mismatch, 1u);
    EXPECT_EQ(book.book().bids().best(0).price, 9151);
}
TEST_F(Refresh, ExhaustionHandsPendingSnapshotToResync) {
    book.mark_requested();
    book.on_snapshot(snapshot(103, 8000, 22000));
    auto exhausted = diff(103);
    for (std::int64_t i = 850; i < 1000; ++i) {
        exhausted.bids.push_back({10000 - i, 0});
        exhausted.asks.push_back({20000 + i, 0});
    }
    EXPECT_FALSE(book.on_diff(exhausted).row);
    EXPECT_EQ(counters.coverage_exhausted, 1u);
    EXPECT_EQ(book.state(), SyncState::Unsynced);
    EXPECT_FALSE(book.snapshot_wanted());
    const auto e = book.on_diff(diff(104));
    EXPECT_TRUE(e.row);
    EXPECT_EQ(e.type, 'R');
    EXPECT_EQ(book.book().bids().best(0).price, 8000);
    EXPECT_EQ(counters.resyncs, 1u);
}
TEST(RefreshProperty, BoundedDepletionKeepsTrueTopsAndNeverExhausts) {
    // Independent full-book oracle: it knows 20,000 levels; the implementation sees 1,000.
    for (const auto venue : {Venue::Spot, Venue::Usdm}) {
        std::map<std::int64_t, std::int64_t, std::greater<>> bids;
        std::map<std::int64_t, std::int64_t> asks;
        for (std::int64_t i = 0; i < 20000; ++i) {
            bids[60000 - i] = 1;
            asks[80000 + i] = 1;
        }
        auto snap = [&](std::uint64_t id) {
            auto s = PayloadParser::make_snapshot();
            s.last_update_id = id;
            auto b = bids.begin();
            auto a = asks.begin();
            for (int i = 0; i < 1000; ++i, ++b, ++a) {
                s.bids.push_back({b->first, b->second});
                s.asks.push_back({a->first, a->second});
            }
            return s;
        };
        SymbolCounters counts;
        SymbolBook book(venue, counts);
        book.enter_unsynced(UnsyncCause::NewEpoch);
        auto bridge = diff(101);
        bridge.first_id = venue == Venue::Spot ? 101 : 100;
        book.on_diff(bridge);
        ASSERT_TRUE(book.on_snapshot(snap(100)).row);
        std::optional<std::pair<int, DepthSnapshot>> pending;
        for (int step = 0; step < 200; ++step) {
            auto d = diff(static_cast<std::uint64_t>(102 + step));
            for (int i = 0; i < 50; ++i) {
                d.bids.push_back({bids.begin()->first, 0});
                bids.erase(bids.begin());
                d.asks.push_back({asks.begin()->first, 0});
                asks.erase(asks.begin());
            }
            ASSERT_TRUE(book.on_diff(d).row) << step;
            if (pending && pending->first == step) {
                ASSERT_TRUE(book.on_snapshot(pending->second).row);
                pending.reset();
            }
            if (book.snapshot_wanted()) {
                ASSERT_GE(book.book().margin(Side::Bid), 150u);
                book.mark_requested();
                pending = {{step + 2, snap(d.last_id)}};
            }
            TopOfBook top;
            book.book().top5(top);
            auto b = bids.begin();
            auto a = asks.begin();
            for (std::size_t i = 0; i < 5; ++i, ++b, ++a) {
                EXPECT_EQ(top.bids[i], (Level{b->first, b->second}));
                EXPECT_EQ(top.asks[i], (Level{a->first, a->second}));
            }
        }
        EXPECT_EQ(counts.coverage_exhausted, 0u);
        EXPECT_GT(counts.refresh_ok, 0u);
        EXPECT_EQ(counts.refresh_mismatch, 0u);
        auto all = diff(302);
        // An unbounded burst really exhausts coverage, even with refresh enabled.
        for (const auto& l : bids)
            all.bids.push_back({l.first, 0});
        EXPECT_FALSE(book.on_diff(all).row);
        EXPECT_EQ(counts.coverage_exhausted, 1u);
    }
}
}  // namespace
}  // namespace blc
