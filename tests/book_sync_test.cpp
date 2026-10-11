#include "blc/book/symbol_book.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

DepthDiff diff(std::uint64_t U, std::uint64_t u, std::vector<Level> bids = {},
               std::vector<Level> asks = {}, std::uint64_t pu = 0) {
    DepthDiff d;
    d.first_id = U;
    d.last_id = u;
    d.prev_last_id = pu;
    d.has_prev = pu != 0;
    d.bids = std::move(bids);
    d.asks = std::move(asks);
    return d;
}

// Full-depth snapshot: 1000 bids 100000..99001 and 1000 asks 100001..101000, qty 1.
DepthSnapshot full_snapshot(std::uint64_t L) {
    DepthSnapshot s;
    s.last_update_id = L;
    for (std::int64_t i = 0; i < 1000; ++i) {
        s.bids.push_back(Level{100'000 - i, 1});
        s.asks.push_back(Level{100'001 + i, 1});
    }
    return s;
}

Depth5 depth5_from(const OrderBook& book, std::uint64_t id) {
    TopOfBook top;
    book.top5(top);
    Depth5 d{};
    d.id = id;
    d.bids = top.bids;
    d.asks = top.asks;
    d.n_bids = 5;
    d.n_asks = 5;
    return d;
}

bool is_row(const Emit& e, char type) {
    return e.row && e.type == type;
}

// A spot book synchronised at id 1000.
struct SpotSynced {
    SymbolCounters counters;
    SymbolBook book{Venue::Spot, counters};
    SpotSynced() {
        book.observe_epoch(0);
        book.enter_unsynced(UnsyncCause::NewEpoch);
        book.on_diff(diff(999, 1000));
        EXPECT_TRUE(is_row(book.on_snapshot(full_snapshot(998)), 'R'));
        EXPECT_EQ(book.state(), SyncState::Synced);
        EXPECT_EQ(book.book().id, 1000u);
    }
};

// A usdm book synchronised at id 1000.
struct UsdmSynced {
    SymbolCounters counters;
    SymbolBook book{Venue::Usdm, counters};
    UsdmSynced() {
        book.observe_epoch(0);
        book.enter_unsynced(UnsyncCause::NewEpoch);
        book.on_diff(diff(990, 1000, {}, {}, 980));
        EXPECT_TRUE(is_row(book.on_snapshot(full_snapshot(995)), 'R'));
        EXPECT_EQ(book.book().id, 1000u);
    }
};

TEST(Sync, SpotHappyPathEmitsR) {
    SymbolCounters c;
    SymbolBook b(Venue::Spot, c);
    b.observe_epoch(0);
    b.enter_unsynced(UnsyncCause::NewEpoch);
    EXPECT_FALSE(b.on_diff(diff(101, 103, {{100'000, 5}})).row);
    EXPECT_FALSE(b.on_diff(diff(104, 106, {}, {{100'001, 7}})).row);
    const Emit e = b.on_snapshot(full_snapshot(103));
    EXPECT_TRUE(e.row);
    EXPECT_EQ(e.type, 'R');
    EXPECT_EQ(e.side, 'N');
    EXPECT_EQ(b.book().id, 106u);
    EXPECT_EQ(b.book().bids().qty_at(100'000), 1);  // [101,103] was inside the snapshot
    EXPECT_EQ(b.book().asks().qty_at(100'001), 7);
    EXPECT_EQ(c.syncs, 1u);
    EXPECT_EQ(c.resyncs, 0u);
}

TEST(Sync, SyncCompletedByLaterDiffEmitsROnThatDiff) {
    SymbolCounters c;
    SymbolBook b(Venue::Spot, c);
    b.observe_epoch(0);
    b.enter_unsynced(UnsyncCause::NewEpoch);
    b.on_diff(diff(101, 105));
    EXPECT_FALSE(b.on_snapshot(full_snapshot(110)).row);
    EXPECT_EQ(b.state(), SyncState::Unsynced);
    const Emit e = b.on_diff(diff(106, 112));
    EXPECT_TRUE(is_row(e, 'R'));
    EXPECT_EQ(b.book().id, 112u);
}

TEST(Sync, SnapshotTooOldRefetch) {
    SymbolCounters c;
    SymbolBook b(Venue::Spot, c);
    b.observe_epoch(0);
    b.enter_unsynced(UnsyncCause::NewEpoch);
    b.mark_requested();
    b.on_diff(diff(101, 105));
    EXPECT_FALSE(b.on_snapshot(full_snapshot(90)).row);
    EXPECT_EQ(c.snapshot_too_old, 1u);
    EXPECT_TRUE(b.snapshot_wanted());
}

TEST(Sync, UsdmBridgeBoundary) {
    SymbolCounters c;
    SymbolBook b(Venue::Usdm, c);
    b.observe_epoch(0);
    b.enter_unsynced(UnsyncCause::NewEpoch);
    b.on_diff(diff(100, 110, {}, {}, 95));
    EXPECT_TRUE(is_row(b.on_snapshot(full_snapshot(100)), 'R'));  // U == L

    SymbolCounters c2;
    SymbolBook b2(Venue::Usdm, c2);
    b2.observe_epoch(0);
    b2.enter_unsynced(UnsyncCause::NewEpoch);
    b2.on_diff(diff(101, 110, {}, {}, 95));
    EXPECT_FALSE(b2.on_snapshot(full_snapshot(100)).row);  // U == L + 1 does not bridge
    EXPECT_EQ(c2.snapshot_too_old, 1u);
}

TEST(Sync, GapEntersUnsyncedAndDropsPendingCheck) {
    SpotSynced s;
    s.book.on_depth5(depth5_from(s.book.book(), 1001));  // pending
    EXPECT_FALSE(s.book.on_diff(diff(1005, 1006)).row);  // gap
    EXPECT_EQ(s.book.state(), SyncState::Unsynced);
    EXPECT_EQ(s.counters.gaps, 1u);
    s.book.on_diff(diff(1007, 1020));
    EXPECT_TRUE(is_row(s.book.on_snapshot(full_snapshot(1006)), 'R'));
    EXPECT_EQ(s.book.book().id, 1020u);
    EXPECT_TRUE(is_row(s.book.on_diff(diff(1021, 1022)), 'D'));
    EXPECT_EQ(s.counters.depth5_full_checks + s.counters.depth5_partial_checks, 0u);
    EXPECT_EQ(s.counters.depth5_mismatch, 0u);
    EXPECT_EQ(s.counters.resyncs, 1u);
}

TEST(Sync, StaleDiffWhileSyncedEmitsD) {
    SpotSynced s;
    const Emit e = s.book.on_diff(diff(990, 1000, {{100'000, 9}}));
    EXPECT_TRUE(is_row(e, 'D'));
    EXPECT_EQ(s.book.book().bids().qty_at(100'000), 1);  // unchanged
    EXPECT_EQ(s.counters.stale_diffs, 1u);
}

TEST(Sync, DiffSideLetter) {
    SpotSynced s;
    EXPECT_EQ(s.book.on_diff(diff(1001, 1001, {{99'500, 2}})).side, 'B');
    EXPECT_EQ(s.book.on_diff(diff(1002, 1002, {}, {{100'500, 2}})).side, 'S');
    EXPECT_EQ(s.book.on_diff(diff(1003, 1003, {{99'500, 3}}, {{100'500, 3}})).side, 'N');
    EXPECT_EQ(s.book.on_diff(diff(1004, 1004)).side, 'N');
}

TEST(Sync, TradeSideIsMakerSide) {
    SpotSynced s;
    Emit e = s.book.on_trade(Trade{true}, true);
    EXPECT_TRUE(is_row(e, 'T'));
    EXPECT_EQ(e.side, 'B');
    e = s.book.on_trade(Trade{false}, true);
    EXPECT_EQ(e.side, 'S');
    EXPECT_FALSE(s.book.on_trade(Trade{true}, false).row);
}

TEST(Sync, CoverageExhaustionNoRow) {
    SpotSynced s;
    std::vector<Level> deletes;
    for (std::int64_t p = 100'000; p > 99'004; --p) {  // 996 best bids
        deletes.push_back(Level{p, 0});
    }
    EXPECT_FALSE(s.book.on_diff(diff(1001, 1001, deletes)).row);
    EXPECT_EQ(s.book.state(), SyncState::Unsynced);
    EXPECT_EQ(s.counters.coverage_exhausted, 1u);
    EXPECT_TRUE(s.book.snapshot_wanted());
}

TEST(Sync, Depth5MismatchResyncs) {
    SpotSynced s;
    Depth5 d5 = depth5_from(s.book.book(), 1000);
    d5.bids[1].qty = 42;
    EXPECT_FALSE(s.book.on_depth5(d5).row);
    EXPECT_EQ(s.book.state(), SyncState::Unsynced);
    EXPECT_EQ(s.counters.depth5_mismatch, 1u);
    EXPECT_EQ(s.counters.depth5_full_checks, 1u);
}

TEST(Sync, Depth5MatchEmitsP) {
    SpotSynced s;
    const Emit e = s.book.on_depth5(depth5_from(s.book.book(), 1000));
    EXPECT_TRUE(is_row(e, 'P'));
    EXPECT_EQ(e.side, 'N');
    EXPECT_EQ(s.counters.depth5_verified, 1u);
    EXPECT_EQ(s.counters.depth5_full_levels_compared, 10u);
}

TEST(Sync, Depth5PendingResolutions) {
    UsdmSynced s;
    // p < U (usdm ids skip): full check before applying d.
    s.book.on_depth5(depth5_from(s.book.book(), 1002));
    EXPECT_TRUE(is_row(s.book.on_diff(diff(1005, 1010, {{99'500, 2}}, {}, 1000)), 'D'));
    EXPECT_EQ(s.counters.depth5_full_checks, 1u);
    EXPECT_EQ(s.counters.depth5_verified, 1u);
    // U <= p < u: partial check before applying d.
    s.book.on_depth5(depth5_from(s.book.book(), 1012));
    EXPECT_TRUE(is_row(s.book.on_diff(diff(1011, 1015, {{99'400, 2}}, {}, 1010)), 'D'));
    EXPECT_EQ(s.counters.depth5_partial_checks, 1u);
    EXPECT_EQ(s.counters.depth5_partially_verified, 1u);
    EXPECT_EQ(s.counters.depth5_partial_levels_compared, 10u);
    // p == u: full check after applying d.
    s.book.on_depth5(depth5_from(s.book.book(), 1020));
    s.book.on_diff(diff(1016, 1020, {{99'300, 2}}, {}, 1015));
    EXPECT_EQ(s.counters.depth5_full_checks, 2u);
    EXPECT_EQ(s.counters.depth5_verified, 2u);
    // p > u: stays pending.
    s.book.on_depth5(depth5_from(s.book.book(), 1030));
    s.book.on_diff(diff(1021, 1025, {}, {}, 1020));
    EXPECT_EQ(s.counters.depth5_full_checks + s.counters.depth5_partial_checks, 3u);
    s.book.on_diff(diff(1026, 1030, {}, {}, 1025));
    EXPECT_EQ(s.counters.depth5_full_checks, 3u);
    EXPECT_EQ(s.counters.depth5_mismatch, 0u);
}

TEST(Sync, EpochFirstRecordResetsAndRequests) {
    SymbolCounters c;
    SymbolBook b(Venue::Spot, c);
    EXPECT_FALSE(b.snapshot_wanted());
    EXPECT_EQ(b.observe_epoch(0), SymbolBook::EpochCheck::Reset);
    b.enter_unsynced(UnsyncCause::NewEpoch);
    EXPECT_TRUE(b.snapshot_wanted());
    EXPECT_EQ(c.resets, 1u);
    EXPECT_EQ(b.observe_epoch(0), SymbolBook::EpochCheck::Same);
    EXPECT_EQ(b.observe_epoch(1), SymbolBook::EpochCheck::Reset);
}

TEST(Sync, EpochRegressionCounted) {
    SymbolCounters c;
    SymbolBook b(Venue::Spot, c);
    b.observe_epoch(2);
    EXPECT_EQ(b.observe_epoch(1), SymbolBook::EpochCheck::Regression);
    EXPECT_EQ(c.epoch_regressions, 1u);
}

TEST(Sync, NewEpochResetsSyncedBook) {
    SpotSynced s;
    EXPECT_EQ(s.book.observe_epoch(1), SymbolBook::EpochCheck::Reset);
    s.book.enter_unsynced(UnsyncCause::NewEpoch);
    EXPECT_EQ(s.book.state(), SyncState::Unsynced);
    EXPECT_EQ(s.book.window().size(), 0u);
    EXPECT_EQ(s.book.book().bids().size(), 0u);
}

TEST(Sync, UnsyncedIgnoresDepth5AndTrades) {
    SymbolCounters c;
    SymbolBook b(Venue::Spot, c);
    b.observe_epoch(0);
    b.enter_unsynced(UnsyncCause::NewEpoch);
    EXPECT_FALSE(b.on_depth5(Depth5{}).row);
    EXPECT_FALSE(b.on_trade(Trade{true}, true).row);
    EXPECT_EQ(c.depth5_stale + c.depth5_unchecked, 0u);
}

TEST(Sync, DiffParseErrorIsGap) {
    SpotSynced s;
    EXPECT_FALSE(s.book.on_diff_error(UnsyncCause::DiffParseError).row);
    EXPECT_EQ(s.book.state(), SyncState::Unsynced);
    EXPECT_EQ(s.book.window().size(), 0u);
    EXPECT_EQ(s.counters.parse_errors, 1u);
    EXPECT_EQ(s.counters.oversized_events, 0u);
}

TEST(Sync, CoverageExhaustionDropsPendingCheck) {
    SpotSynced s;
    s.book.on_depth5(depth5_from(s.book.book(), 1001));
    std::vector<Level> deletes;
    for (std::int64_t p = 100'000; p > 99'004; --p) {
        deletes.push_back(Level{p, 0});
    }
    EXPECT_FALSE(s.book.on_diff(diff(1001, 1001, deletes)).row);
    EXPECT_EQ(s.book.state(), SyncState::Unsynced);
    EXPECT_EQ(s.counters.depth5_full_checks + s.counters.depth5_partial_checks, 0u);
}

TEST(Sync, NoDuplicateRequestWhileOutstanding) {
    SymbolCounters c;
    SymbolBook b(Venue::Spot, c);
    b.observe_epoch(0);
    b.enter_unsynced(UnsyncCause::NewEpoch);
    ASSERT_TRUE(b.snapshot_wanted());
    b.mark_requested();
    EXPECT_FALSE(b.snapshot_wanted());
    b.on_diff(diff(101, 105));
    b.on_diff_error(UnsyncCause::DiffParseError);
    EXPECT_FALSE(b.snapshot_wanted());
    b.on_diff(diff(106, 110));
    b.on_snapshot(full_snapshot(90));  // too old
    EXPECT_TRUE(b.snapshot_wanted());
}

TEST(Sync, SnapshotErrorRerequests) {
    SymbolCounters c;
    SymbolBook b(Venue::Spot, c);
    b.observe_epoch(0);
    b.enter_unsynced(UnsyncCause::NewEpoch);
    b.mark_requested();
    b.on_snapshot_error(false);
    EXPECT_TRUE(b.snapshot_wanted());
    EXPECT_EQ(c.parse_errors, 1u);
    EXPECT_EQ(c.oversized_events, 0u);
    b.mark_requested();
    b.on_snapshot_error(true);
    EXPECT_EQ(c.parse_errors, 2u);
    EXPECT_EQ(c.oversized_events, 1u);
}

TEST(Sync, Depth5StaleAndPendingReplacementCounted) {
    SpotSynced s;
    EXPECT_TRUE(is_row(s.book.on_depth5(depth5_from(s.book.book(), 999)), 'P'));
    EXPECT_EQ(s.counters.depth5_stale, 1u);
    Depth5 older = depth5_from(s.book.book(), 1001);
    older.bids[0].qty = 77;  // would mismatch if it were ever checked
    s.book.on_depth5(older);
    s.book.on_depth5(depth5_from(s.book.book(), 1001));
    EXPECT_EQ(s.counters.depth5_unchecked, 1u);
    s.book.on_diff(diff(1001, 1001, {{99'000, 3}}));
    EXPECT_EQ(s.counters.depth5_verified, 1u);
    EXPECT_EQ(s.counters.depth5_mismatch, 0u);
}

TEST(Sync, OversizedDiffCountsAndResets) {
    SpotSynced s;
    s.book.on_depth5(depth5_from(s.book.book(), 1001));
    EXPECT_FALSE(s.book.on_diff_error(UnsyncCause::Oversized).row);
    EXPECT_EQ(s.book.state(), SyncState::Unsynced);
    EXPECT_EQ(s.book.window().size(), 0u);
    EXPECT_EQ(s.counters.parse_errors, 1u);
    EXPECT_EQ(s.counters.oversized_events, 1u);
}

TEST(Sync, CountOnlyPolicyDoesNotResync) {
    SymbolCounters c;
    SymbolBook b(Venue::Spot, c, Depth5Policy::CountOnly);
    b.observe_epoch(0);
    b.enter_unsynced(UnsyncCause::NewEpoch);
    b.on_diff(diff(999, 1000));
    ASSERT_TRUE(is_row(b.on_snapshot(full_snapshot(998)), 'R'));
    Depth5 d5 = depth5_from(b.book(), 1000);
    d5.asks[0].qty = 99;
    EXPECT_TRUE(is_row(b.on_depth5(d5), 'P'));
    EXPECT_EQ(c.depth5_mismatch, 1u);
    EXPECT_EQ(b.state(), SyncState::Synced);
    EXPECT_EQ(depth5_policy(Venue::Spot), Depth5Policy::Resync);
    EXPECT_EQ(depth5_policy(Venue::Usdm), Depth5Policy::Resync);
}

TEST(Sync, NonAuthoritativeBootstrapDiscardsSnapshot) {
    SymbolCounters c;
    SymbolBook b(Venue::Spot, c);
    b.observe_epoch(0);
    b.enter_unsynced(UnsyncCause::NewEpoch);
    b.mark_requested();
    std::vector<Level> deletes;
    for (std::int64_t p = 100'000; p > 99'004; --p) {
        deletes.push_back(Level{p, 0});
    }
    b.on_diff(diff(101, 105, deletes));
    EXPECT_FALSE(b.on_snapshot(full_snapshot(100)).row);
    EXPECT_EQ(b.state(), SyncState::Unsynced);
    EXPECT_TRUE(b.snapshot_wanted());
}

TEST(Sync, SnapshotWhileSyncedIsIgnored) {
    SpotSynced s;
    EXPECT_FALSE(s.book.on_snapshot(full_snapshot(2000)).row);
    EXPECT_EQ(s.book.state(), SyncState::Synced);
    EXPECT_EQ(s.book.book().id, 1000u);
}

}  // namespace
}  // namespace blc
