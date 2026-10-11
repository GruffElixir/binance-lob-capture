#pragma once

#include "blc/book/order_book.hpp"
#include "blc/csv/csv_writer.hpp"

#include <cstdint>

namespace blc {

// Writes one 26-column order-book row (design §3.4), including its terminator.
void write_book_row(CsvRow& row, std::int64_t recv_ns, std::uint64_t seq, std::int32_t id,
                    char type, char side, const TopOfBook& top) noexcept;

}  // namespace blc
