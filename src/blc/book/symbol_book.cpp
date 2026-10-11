#include "blc/book/symbol_book.hpp"

#include "blc/book/sequence.hpp"
#include "blc/parse/payload_parser.hpp"

#include <ostream>

namespace blc {
namespace {

struct MismatchContext {
    const OrderBook* book;
    const Depth5* d5;
};

void write_levels(std::ostream& os, const char* name, const Level* levels, std::size_t n) {
    os << ' ' << name << '[';
    for (std::size_t i = 0; i < n; ++i) {
        os << (i ? " " : "") << levels[i].price << '@' << levels[i].qty;
    }
    os << ']';
}

void format_mismatch(std::ostream& os, const void* raw) {
    const auto& ctx = *static_cast<const MismatchContext*>(raw);
    TopOfBook top;
    ctx.book->top5(top);
    os << "depth5 mismatch: book id=" << ctx.book->id << " depth5 id=" << ctx.d5->id;
    write_levels(os, "book_bids", top.bids.data(), top.bids.size());
    write_levels(os, "book_asks", top.asks.data(), top.asks.size());
    write_levels(os, "depth5_bids", ctx.d5->bids.data(), ctx.d5->n_bids);
    write_levels(os, "depth5_asks", ctx.d5->asks.data(), ctx.d5->n_asks);
}

}  // namespace

SymbolBook::SymbolBook(Venue venue, SymbolCounters& counters)
    : SymbolBook(venue, counters, depth5_policy(venue)) {}

SymbolBook::SymbolBook(Venue venue, SymbolCounters& counters, Depth5Policy policy)
    : venue_(venue),
      counters_(counters),
      policy_(policy),
      pending_snapshot_(PayloadParser::make_snapshot()) {}

SymbolBook::EpochCheck SymbolBook::observe_epoch(std::uint32_t epoch) noexcept {
    if (!last_epoch_ || epoch > *last_epoch_) {
        last_epoch_ = epoch;
        return EpochCheck::Reset;
    }
    if (epoch < *last_epoch_) {
        ++counters_.epoch_regressions;
        return EpochCheck::Regression;
    }
    return EpochCheck::Same;
}

void SymbolBook::enter_unsynced(UnsyncCause cause) noexcept {
    book_.clear();
    has_pending_check_ = false;
    state_ = SyncState::Unsynced;
    needs_snapshot_ = true;
    switch (cause) {
        case UnsyncCause::NewEpoch:
            window_.clear();
            has_pending_snapshot_ = false;
            outstanding_ = false;  // a new epoch needs a request of its own
            ++counters_.resets;
            break;
        case UnsyncCause::Gap:
            ++counters_.gaps;  // the caller restarts the window with the gap diff
            break;
        case UnsyncCause::DiffParseError:
        case UnsyncCause::Oversized:
            window_.clear();
            break;
        case UnsyncCause::CoverageExhausted:
            ++counters_.coverage_exhausted;
            break;
        case UnsyncCause::Depth5Mismatch:
            break;
    }
}

void SymbolBook::append_to_window(const DepthDiff& diff) noexcept {
    switch (window_.append(venue_, diff)) {
        case DiffWindow::Append::Stale:
            ++counters_.stale_diffs;
            break;
        case DiffWindow::Append::Reset:
            ++counters_.window_resets;
            break;
        case DiffWindow::Append::Appended:
            break;
    }
    counters_.window_trimmed += window_.trimmed() - window_trimmed_seen_;
    window_trimmed_seen_ = window_.trimmed();
}

char SymbolBook::diff_side(const DepthDiff& diff) noexcept {
    if (!diff.bids.empty() && diff.asks.empty()) {
        return 'B';
    }
    if (diff.bids.empty() && !diff.asks.empty()) {
        return 'S';
    }
    return 'N';
}

Emit SymbolBook::try_sync() noexcept {
    if (!has_pending_snapshot_ || window_.empty()) {
        return {};
    }
    const std::uint64_t L = pending_snapshot_.last_update_id;
    std::size_t first = window_.size();
    for (std::size_t i = 0; i < window_.size(); ++i) {
        if (!contained_in_snapshot(venue_, window_.at(i).u, L)) {
            first = i;
            break;
        }
    }
    if (first == window_.size()) {
        return {};  // the snapshot is newer than every buffered diff: wait for more
    }
    const DiffView bridge = window_.at(first);
    if (!bridges(venue_, bridge.U, bridge.u, L)) {
        ++counters_.snapshot_too_old;
        has_pending_snapshot_ = false;
        needs_snapshot_ = true;
        return {};
    }
    book_.load(pending_snapshot_);
    for (std::size_t i = first; i < window_.size(); ++i) {
        const DiffView d = window_.at(i);
        book_.apply(d.bids, d.asks);
        book_.id = d.u;
    }
    has_pending_snapshot_ = false;
    if (!book_.authoritative()) {
        book_.clear();
        needs_snapshot_ = true;  // a fresh snapshot is needed
        return {};
    }
    state_ = SyncState::Synced;
    needs_snapshot_ = false;
    ++counters_.syncs;
    if (counters_.syncs > 1) {
        ++counters_.resyncs;
    }
    return Emit{true, 'R', 'N'};
}

bool SymbolBook::handle_result(CheckResult result, const Depth5& d5) noexcept {
    switch (result) {
        case CheckResult::Verified:
            ++counters_.depth5_verified;
            return true;
        case CheckResult::PartiallyVerified:
            ++counters_.depth5_partially_verified;
            return true;
        case CheckResult::Mismatch:
            break;
    }
    ++counters_.depth5_mismatch;
    if (mismatch_log_.should_log()) {
        const MismatchContext ctx{&book_, &d5};
        log_formatted(LogLevel::Warn, &format_mismatch, &ctx);
    }
    return policy_ == Depth5Policy::CountOnly;
}

bool SymbolBook::full_check_now(const Depth5& d5) noexcept {
    std::size_t compared = 0;
    const CheckResult r = full_check(book_, d5, compared);
    ++counters_.depth5_full_checks;
    counters_.depth5_full_levels_compared += compared;
    return handle_result(r, d5);
}

bool SymbolBook::partial_check_now(const Depth5& d5, const DiffView& d) noexcept {
    std::size_t unchecked = 0;
    std::size_t compared = 0;
    const CheckResult r = partial_check(book_, d5, d, unchecked, compared);
    ++counters_.depth5_partial_checks;
    counters_.depth5_partial_levels_compared += compared;
    counters_.depth5_levels_unchecked += unchecked;
    return handle_result(r, d5);
}

Emit SymbolBook::on_diff(const DepthDiff& diff) noexcept {
    if (state_ == SyncState::Unsynced) {
        append_to_window(diff);
        return try_sync();
    }

    switch (classify(venue_, diff.first_id, diff.last_id, diff.prev_last_id, book_.id)) {
        case SeqClass::Stale:
            ++counters_.stale_diffs;
            return Emit{true, 'D', diff_side(diff)};
        case SeqClass::Gap:
            window_.restart_with(diff);
            enter_unsynced(UnsyncCause::Gap);
            return {};
        case SeqClass::Continues:
            break;
    }

    append_to_window(diff);
    const DiffView view = view_of(diff);

    // Resolve a pending depth5 check against the state before d where §5.6 says so.
    bool check_after = false;
    if (has_pending_check_) {
        const std::uint64_t p = pending_check_.id;
        if (p <= book_.id) {
            ++counters_.depth5_unchecked;
            has_pending_check_ = false;
        } else if (p < diff.first_id) {
            has_pending_check_ = false;
            if (!full_check_now(pending_check_)) {
                enter_unsynced(UnsyncCause::Depth5Mismatch);
                return {};
            }
        } else if (p < diff.last_id) {
            has_pending_check_ = false;
            if (!partial_check_now(pending_check_, view)) {
                enter_unsynced(UnsyncCause::Depth5Mismatch);
                return {};
            }
        } else if (p == diff.last_id) {
            check_after = true;
        }
    }

    book_.apply(view.bids, view.asks);
    book_.id = diff.last_id;

    if (!book_.authoritative()) {
        enter_unsynced(UnsyncCause::CoverageExhausted);  // also drops the pending check
        return {};
    }
    if (check_after) {
        has_pending_check_ = false;
        if (!full_check_now(pending_check_)) {
            enter_unsynced(UnsyncCause::Depth5Mismatch);
            return {};
        }
    }
    return Emit{true, 'D', diff_side(diff)};
}

Emit SymbolBook::on_diff_error(UnsyncCause cause) noexcept {
    ++counters_.parse_errors;
    if (cause == UnsyncCause::Oversized) {
        ++counters_.oversized_events;
    }
    enter_unsynced(cause);
    return {};
}

Emit SymbolBook::on_depth5(const Depth5& d5) noexcept {
    if (state_ != SyncState::Synced) {
        return {};
    }
    if (d5.id < book_.id) {
        ++counters_.depth5_stale;
    } else if (d5.id == book_.id) {
        if (!full_check_now(d5)) {
            enter_unsynced(UnsyncCause::Depth5Mismatch);
            return {};
        }
    } else {
        if (has_pending_check_) {
            ++counters_.depth5_unchecked;
        }
        pending_check_ = d5;
        has_pending_check_ = true;
    }
    return Emit{true, 'P', 'N'};
}

Emit SymbolBook::on_trade(const Trade& trade, bool trade_rows) noexcept {
    if (state_ != SyncState::Synced || !trade_rows) {
        return {};
    }
    // Book side of the resting (maker) order: buyer is maker -> the bid side.
    return Emit{true, 'T', trade.buyer_is_maker ? 'B' : 'S'};
}

Emit SymbolBook::on_snapshot(const DepthSnapshot& snapshot) noexcept {
    outstanding_ = false;
    if (state_ == SyncState::Synced) {
        return {};  // only a background refresh (M7) would use it
    }
    pending_snapshot_.last_update_id = snapshot.last_update_id;
    pending_snapshot_.bids.assign(snapshot.bids.begin(), snapshot.bids.end());
    pending_snapshot_.asks.assign(snapshot.asks.begin(), snapshot.asks.end());
    has_pending_snapshot_ = true;
    return try_sync();
}

Emit SymbolBook::on_snapshot_error(bool oversized) noexcept {
    ++counters_.parse_errors;
    if (oversized) {
        ++counters_.oversized_events;
    }
    outstanding_ = false;
    if (state_ == SyncState::Unsynced) {
        needs_snapshot_ = true;
    }
    return {};
}

}  // namespace blc
