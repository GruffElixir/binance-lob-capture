#include "blc/pipeline/row_format.hpp"

#include "blc/core/time.hpp"

namespace blc {

void write_book_row(CsvRow& row, std::int64_t recv_ns, std::uint64_t seq, std::int32_t id,
                    char type, char side, const TopOfBook& top) noexcept {
    const SplitTime t = split_ns(recv_ns);
    row.integer(t.sec).integer(t.nsec).unsigned_integer(seq).integer(id).character(type).character(
        side);
    for (const Level& l : top.bids) {
        row.integer(l.price);
    }
    for (const Level& l : top.bids) {
        row.integer(l.qty);
    }
    for (const Level& l : top.asks) {
        row.integer(l.price);
    }
    for (const Level& l : top.asks) {
        row.integer(l.qty);
    }
    row.end();
}

}  // namespace blc
