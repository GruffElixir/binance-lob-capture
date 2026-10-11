#include "blc/book/diff_window.hpp"

#include "blc/book/sequence.hpp"

#include <algorithm>

namespace blc {

DiffView view_of(const DepthDiff& diff) noexcept {
    return DiffView{diff.first_id, diff.last_id, diff.prev_last_id, span_of(diff.bids),
                    span_of(diff.asks)};
}

DiffWindow::DiffWindow(std::size_t max_levels, std::size_t max_diffs)
    : levels_(max_levels), headers_(max_diffs) {}

DiffWindow::Append DiffWindow::append(Venue venue, const DepthDiff& diff) noexcept {
    if (count_ == 0) {
        store(diff);
        return Append::Appended;
    }
    const Header& last = header(count_ - 1);
    switch (classify(venue, diff.first_id, diff.last_id, diff.prev_last_id, last.u)) {
        case SeqClass::Stale:
            return Append::Stale;
        case SeqClass::Gap:
            restart_with(diff);
            return Append::Reset;
        case SeqClass::Continues:
            store(diff);
            return Append::Appended;
    }
    return Append::Stale;
}

void DiffWindow::restart_with(const DepthDiff& diff) noexcept {
    clear();
    store(diff);
}

void DiffWindow::clear() noexcept {
    first_ = 0;
    count_ = 0;
    begin_ = 0;
    end_ = 0;
}

void DiffWindow::drop_oldest() noexcept {
    const Header& h = headers_[first_];
    begin_ = h.offset + h.n_bids + h.n_asks;
    first_ = (first_ + 1) % headers_.size();
    --count_;
    ++trimmed_;
    if (count_ == 0) {
        begin_ = 0;
        end_ = 0;
    }
}

void DiffWindow::store(const DepthDiff& diff) noexcept {
    const std::size_t n = diff.bids.size() + diff.asks.size();
    if (n > levels_.size() || headers_.empty()) {
        clear();  // cannot be represented (the parser rejects such diffs before this point)
        return;
    }
    while (count_ > 0 && (count_ == headers_.size() || (end_ - begin_) + n > levels_.size())) {
        drop_oldest();
    }
    if (end_ + n > levels_.size()) {
        // Compact the live region to the front so the new diff is contiguous.
        std::copy(levels_.begin() + static_cast<std::ptrdiff_t>(begin_),
                  levels_.begin() + static_cast<std::ptrdiff_t>(end_), levels_.begin());
        for (std::size_t i = 0; i < count_; ++i) {
            headers_[(first_ + i) % headers_.size()].offset -= begin_;
        }
        end_ -= begin_;
        begin_ = 0;
    }
    const std::size_t offset = end_;
    std::copy(diff.bids.begin(), diff.bids.end(),
              levels_.begin() + static_cast<std::ptrdiff_t>(offset));
    std::copy(diff.asks.begin(), diff.asks.end(),
              levels_.begin() + static_cast<std::ptrdiff_t>(offset + diff.bids.size()));
    end_ += n;
    headers_[(first_ + count_) % headers_.size()] =
        Header{diff.first_id,
               diff.last_id,
               diff.prev_last_id,
               offset,
               static_cast<std::uint32_t>(diff.bids.size()),
               static_cast<std::uint32_t>(diff.asks.size())};
    ++count_;
}

DiffView DiffWindow::at(std::size_t i) const noexcept {
    const Header& h = header(i);
    const Level* base = levels_.data() + h.offset;
    return DiffView{h.U, h.u, h.pu, LevelSpan{base, h.n_bids},
                    LevelSpan{base + h.n_bids, h.n_asks}};
}

}  // namespace blc
