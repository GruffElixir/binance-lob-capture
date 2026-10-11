#pragma once

#include "blc/book/depth5_check.hpp"
#include "blc/book/diff_window.hpp"
#include "blc/book/order_book.hpp"
#include "blc/parse/events.hpp"
#include "blc/util/logging.hpp"

#include <cstdint>
#include <optional>

namespace blc {

enum class SyncState { Unsynced, Synced };

enum class UnsyncCause {
    NewEpoch,
    Gap,
    DiffParseError,
    Oversized,
    CoverageExhausted,
    Depth5Mismatch
};

// What the processor writes for the record just processed (design §5.8).
struct Emit {
    bool row = false;
    char type = 0;  // 'R', 'D', 'P' or 'T'
    char side = 'N';
};

struct SymbolCounters {
    std::uint64_t stale_diffs = 0;
    std::uint64_t gaps = 0;
    std::uint64_t resets = 0;
    std::uint64_t syncs = 0;
    std::uint64_t resyncs = 0;
    std::uint64_t snapshot_too_old = 0;
    std::uint64_t coverage_exhausted = 0;
    std::uint64_t window_resets = 0;
    std::uint64_t window_trimmed = 0;
    std::uint64_t depth5_verified = 0;
    std::uint64_t depth5_partially_verified = 0;
    std::uint64_t depth5_mismatch = 0;
    std::uint64_t depth5_stale = 0;
    std::uint64_t depth5_unchecked = 0;
    std::uint64_t depth5_levels_unchecked = 0;
    std::uint64_t depth5_full_checks = 0;
    std::uint64_t depth5_partial_checks = 0;
    std::uint64_t depth5_full_levels_compared = 0;
    std::uint64_t depth5_partial_levels_compared = 0;
    std::uint64_t parse_errors = 0;
    std::uint64_t oversized_events = 0;
    std::uint64_t epoch_regressions = 0;
    std::uint64_t refresh_ok = 0;
    std::uint64_t refresh_mismatch = 0;
};

enum class Depth5Policy { Resync, CountOnly };

// Both venues resync on a depth5 mismatch; the 10-minute live measurement found none (§15).
constexpr Depth5Policy depth5_policy(Venue /*venue*/) noexcept {
    return Depth5Policy::Resync;
}

// One symbol's book and its synchronisation state machine (design §5.4-§5.8). Pure logic: no
// timers, no I/O, so its output is a function of its input records only.
class SymbolBook {
public:
    enum class EpochCheck { Same, Reset, Regression };

    SymbolBook(Venue venue, SymbolCounters& counters);
    SymbolBook(Venue venue, SymbolCounters& counters, Depth5Policy policy,
               bool refresh_enabled = true);

    // Tracks the record's connection epoch. Reset: the first record, or a newer epoch; the caller
    // then runs enter_unsynced(NewEpoch). Regression: counted; the caller skips the record.
    EpochCheck observe_epoch(std::uint32_t epoch) noexcept;

    Emit on_diff(const DepthDiff& diff) noexcept;
    Emit on_diff_error(UnsyncCause cause) noexcept;  // DiffParseError or Oversized
    Emit on_depth5(const Depth5& d5) noexcept;
    Emit on_trade(const Trade& trade, bool trade_rows) noexcept;
    Emit on_snapshot(const DepthSnapshot& snapshot) noexcept;
    Emit on_snapshot_error(bool oversized) noexcept;

    bool snapshot_wanted() const noexcept {
        return needs_snapshot_ && !outstanding_ && !has_pending_snapshot_;
    }
    void mark_requested() noexcept { outstanding_ = true; }

    void enter_unsynced(UnsyncCause cause) noexcept;

    SyncState state() const noexcept { return state_; }
    const OrderBook& book() const noexcept { return book_; }
    const DiffWindow& window() const noexcept { return window_; }
    // Capacity of everything this symbol keeps between records.
    std::size_t retained_bytes() const noexcept {
        return book_.retained_bytes() + shadow_.retained_bytes() + window_.retained_bytes() +
               (pending_snapshot_.bids.capacity() + pending_snapshot_.asks.capacity()) *
                   sizeof(Level);
    }

private:
    Emit try_sync() noexcept;
    void consider_refresh() noexcept;
    void append_to_window(const DepthDiff& diff) noexcept;
    // Runs a check; returns false on a mismatch that must resync.
    bool full_check_now(const Depth5& d5) noexcept;
    bool partial_check_now(const Depth5& d5, const DiffView& d) noexcept;
    bool handle_result(CheckResult result, const Depth5& d5) noexcept;
    static char diff_side(const DepthDiff& diff) noexcept;

    Venue venue_;
    SymbolCounters& counters_;
    Depth5Policy policy_;
    SyncState state_ = SyncState::Unsynced;
    OrderBook book_;
    OrderBook shadow_;
    DiffWindow window_;
    std::uint64_t window_trimmed_seen_ = 0;
    DepthSnapshot pending_snapshot_;
    bool has_pending_snapshot_ = false;
    Depth5 pending_check_{};
    bool has_pending_check_ = false;
    std::optional<std::uint32_t> last_epoch_;
    bool needs_snapshot_ = false;
    bool outstanding_ = false;
    bool refresh_enabled_;
    RateLimitedLog mismatch_log_;
};

}  // namespace blc
