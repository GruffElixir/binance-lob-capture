// Exchange-simulator property test (design §13): every emitted row equals the simulator's true
// top 5; output is suppressed only inside unsynced intervals; syncs happen exactly when an
// independent oracle says they must.

#include "blc/book/sequence.hpp"
#include "blc/book/symbol_book.hpp"
#include "support/exchange_sim.hpp"

#include <deque>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

using test::ExchangeSim;

struct RunStats {
    std::uint64_t rows = 0;
    std::uint64_t syncs_on_snapshot = 0;
    std::uint64_t syncs_on_diff = 0;
    std::uint64_t transitions = 0;  // Synced -> Unsynced
    bool final_synced = false;
    SymbolCounters counters;
};

std::string where(Venue v, std::uint32_t seed, int step) {
    std::ostringstream os;
    os << (v == Venue::Spot ? "spot" : "usdm") << " seed=" << seed << " step=" << step;
    return os.str();
}

bool tops_equal(const TopOfBook& a, const TopOfBook& b) {
    return a.bids == b.bids && a.asks == b.asks;
}

// Mirror of the window's contiguity rule, kept independently by the harness.
struct Run {
    struct Entry {
        std::uint64_t U, u;
    };
    std::vector<Entry> entries;
    void add(Venue v, const DepthDiff& d) {
        if (entries.empty()) {
            entries.push_back({d.first_id, d.last_id});
            return;
        }
        switch (classify(v, d.first_id, d.last_id, d.prev_last_id, entries.back().u)) {
            case SeqClass::Stale:
                return;
            case SeqClass::Gap:
                entries.clear();
                entries.push_back({d.first_id, d.last_id});
                return;
            case SeqClass::Continues:
                entries.push_back({d.first_id, d.last_id});
                return;
        }
    }
};

void run_into(RunStats& stats, Venue venue, std::uint32_t seed, int steps,
              ExchangeSim::Options options) {
    ExchangeSim sim(venue, seed, options);
    SymbolBook book(venue, stats.counters);
    std::uint32_t epoch = 0;
    struct Request {
        int deliver_at;
        std::uint32_t epoch;
        DepthSnapshot snap;
    };
    std::deque<Request> requests;
    Run run_log;
    std::optional<DepthSnapshot> pending;   // the book's pending snapshot, per the oracle
    std::optional<DepthSnapshot> accepted;  // the snapshot that established the Synced book

    // Oracle for an Unsynced book holding `pending` after a sync attempt.
    const auto expect_after_attempt = [&](const std::string& ctx) {
        if (!pending) {
            EXPECT_EQ(book.state(), SyncState::Unsynced) << ctx;
            return;
        }
        const std::uint64_t L = pending->last_update_id;
        const Run::Entry* first = nullptr;
        for (const auto& e : run_log.entries) {
            if (!contained_in_snapshot(venue, e.u, L)) {
                first = &e;
                break;
            }
        }
        if (first == nullptr) {
            EXPECT_EQ(book.state(), SyncState::Unsynced) << ctx << " (waiting for a bridge)";
            return;
        }
        const bool should_sync =
            bridges(venue, first->U, first->u, L) && sim.coverage_sufficient(*pending);
        EXPECT_EQ(book.state() == SyncState::Synced, should_sync) << ctx << " L=" << L;
        if (should_sync) {
            accepted = pending;
        }
        pending.reset();  // used or discarded either way
    };

    for (int step = 0; step < steps; ++step) {
        const std::string ctx = where(venue, seed, step);
        ExchangeSim::Step s = sim.next();
        if (s.kind == ExchangeSim::Step::Reconnect) {
            epoch = s.epoch;
        } else {
            const bool was_synced = book.state() == SyncState::Synced;
            if (book.observe_epoch(epoch) == SymbolBook::EpochCheck::Reset) {
                book.enter_unsynced(UnsyncCause::NewEpoch);
                run_log.entries.clear();
                pending.reset();
                if (was_synced) {
                    ++stats.transitions;
                }
            }
            const bool synced_before = book.state() == SyncState::Synced;
            const auto before = stats.counters;
            Emit e;
            switch (s.kind) {
                case ExchangeSim::Step::Diff:
                    run_log.add(venue, s.diff);
                    e = book.on_diff(s.diff);
                    break;
                case ExchangeSim::Step::Depth5:
                    e = book.on_depth5(s.d5);
                    break;
                case ExchangeSim::Step::Trade:
                    e = book.on_trade(s.trade, true);
                    break;
                case ExchangeSim::Step::Reconnect:
                    break;
            }
            const bool synced_after = book.state() == SyncState::Synced;

            if (e.row) {
                ++stats.rows;
                TopOfBook top;
                book.book().top5(top);
                ASSERT_TRUE(tops_equal(top, sim.true_top())) << ctx << " type=" << e.type;
            }
            if (synced_before && synced_after) {
                ASSERT_TRUE(e.row) << ctx << ": a row was withheld outside an unsynced interval";
            }
            ASSERT_EQ(stats.counters.depth5_mismatch, 0u) << ctx;
            if (synced_before && s.kind == ExchangeSim::Step::Diff && accepted) {
                const bool exhausted =
                    stats.counters.coverage_exhausted > before.coverage_exhausted;
                const bool gap = stats.counters.gaps > before.gaps;
                if (!gap) {
                    ASSERT_EQ(exhausted, !sim.coverage_sufficient(*accepted))
                        << ctx << ": coverage exhaustion must match the oracle";
                }
            }
            if (synced_before && !synced_after) {
                ++stats.transitions;
                accepted.reset();
            }
            if (!synced_before && s.kind == ExchangeSim::Step::Diff) {
                expect_after_attempt(ctx + " (diff)");
                if (synced_after) {
                    ASSERT_TRUE(e.row && e.type == 'R') << ctx;
                    ++stats.syncs_on_diff;
                }
            }
            if (book.snapshot_wanted()) {
                book.mark_requested();
                requests.push_back(Request{step + 2, epoch, sim.snapshot()});
            }
        }

        while (!requests.empty() && requests.front().deliver_at <= step) {
            Request r = std::move(requests.front());
            requests.pop_front();
            if (r.epoch != sim.epoch() || r.epoch != epoch) {
                continue;  // a response for an older connection is discarded (FrameRouter rule)
            }
            if (book.observe_epoch(epoch) == SymbolBook::EpochCheck::Reset) {
                book.enter_unsynced(UnsyncCause::NewEpoch);
                run_log.entries.clear();
            }
            const bool synced_before = book.state() == SyncState::Synced;
            if (!synced_before) {
                pending = r.snap;
            }
            const Emit e = book.on_snapshot(r.snap);
            if (synced_before) {
                ASSERT_FALSE(e.row) << where(venue, seed, step);
                continue;
            }
            expect_after_attempt(where(venue, seed, step) + " (snapshot)");
            if (book.state() == SyncState::Synced) {
                ASSERT_TRUE(e.row && e.type == 'R');
                ++stats.rows;
                ++stats.syncs_on_snapshot;
                TopOfBook top;
                book.book().top5(top);
                ASSERT_TRUE(tops_equal(top, sim.true_top())) << where(venue, seed, step);
            }
            if (book.snapshot_wanted()) {
                book.mark_requested();
                requests.push_back(Request{step + 2, epoch, sim.snapshot()});
            }
        }
    }
    stats.final_synced = book.state() == SyncState::Synced;
}

RunStats run(Venue venue, std::uint32_t seed, int steps, ExchangeSim::Options options) {
    RunStats stats;
    run_into(stats, venue, seed, steps, options);
    return stats;
}

void check_consistency(const RunStats& s, const std::string& ctx) {
    EXPECT_EQ(s.counters.syncs, s.transitions + (s.final_synced ? 1u : 0u)) << ctx;
    EXPECT_EQ(s.counters.depth5_mismatch, 0u) << ctx;
}

class BookProperty : public ::testing::TestWithParam<Venue> {};

TEST_P(BookProperty, RowsAlwaysMatchTheTrueBook) {
    const Venue venue = GetParam();
    std::uint64_t on_snapshot = 0;
    std::uint64_t on_diff = 0;
    std::uint64_t checks = 0;
    std::uint64_t gaps = 0;
    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        const RunStats s = run(venue, seed, 10'000, ExchangeSim::Options{});
        if (::testing::Test::HasFatalFailure()) {
            return;
        }
        check_consistency(s, where(venue, seed, -1));
        EXPECT_GT(s.rows, 5'000u);
        on_snapshot += s.syncs_on_snapshot;
        on_diff += s.syncs_on_diff;
        checks += s.counters.depth5_full_checks + s.counters.depth5_partial_checks;
        gaps += s.counters.gaps;
    }
    EXPECT_GT(on_snapshot, 0u) << "sync completed on the snapshot record";
    EXPECT_GT(on_diff, 0u) << "sync completed on a later diff";
    EXPECT_GT(checks, 1'000u) << "depth5 checks actually ran";
    EXPECT_GT(gaps, 0u) << "gaps were exercised";
}

TEST_P(BookProperty, DepletionExhaustsCoverageAndRecovers) {
    const Venue venue = GetParam();
    ExchangeSim::Options o;
    o.p_gap = 0;
    o.p_reconnect = 0;
    o.drift_down = true;
    const RunStats s = run(venue, 99, 400, o);
    check_consistency(s, "depletion");
    EXPECT_GE(s.counters.coverage_exhausted, 1u);
    EXPECT_GE(s.counters.resyncs, 1u);
}

TEST_P(BookProperty, RunEndingBeforeInitialSync) {
    const RunStats s = run(GetParam(), 5, 1, ExchangeSim::Options{});
    EXPECT_FALSE(s.final_synced);
    check_consistency(s, "short");
}

TEST_P(BookProperty, RunEndingDuringResync) {
    // Depletion runs exhaust coverage every ~50 diffs; stop right after the first exhaustion.
    ExchangeSim::Options o;
    o.p_gap = 0;
    o.p_reconnect = 0;
    o.drift_down = true;
    for (int steps = 20; steps < 400; ++steps) {
        const RunStats s = run(GetParam(), 99, steps, o);
        if (s.counters.coverage_exhausted > 0 && !s.final_synced) {
            check_consistency(s, "ends unsynced");
            return;
        }
    }
    FAIL() << "no run ended during a resync";
}

INSTANTIATE_TEST_SUITE_P(Venues, BookProperty, ::testing::Values(Venue::Spot, Venue::Usdm),
                         [](const auto& param_info) {
                             return param_info.param == Venue::Spot ? "Spot" : "Usdm";
                         });

TEST(BookPropertyDeterministic, OldEpochResponseNeverReachesTheBook) {
    SymbolCounters c;
    SymbolBook b(Venue::Spot, c);
    b.observe_epoch(0);
    b.enter_unsynced(UnsyncCause::NewEpoch);
    ASSERT_TRUE(b.snapshot_wanted());
    b.mark_requested();  // request for epoch 0 in flight
    // Reconnect: the first record of epoch 1 resets the symbol and clears the outstanding mark.
    ASSERT_EQ(b.observe_epoch(1), SymbolBook::EpochCheck::Reset);
    b.enter_unsynced(UnsyncCause::NewEpoch);
    EXPECT_TRUE(b.snapshot_wanted());  // a new request for epoch 1 is published
    // The epoch-0 response is discarded by the delivery rule and never passed to the book.
    EXPECT_EQ(b.state(), SyncState::Unsynced);
}

}  // namespace
}  // namespace blc
