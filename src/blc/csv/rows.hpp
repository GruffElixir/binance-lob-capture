#pragma once

#include "blc/core/types.hpp"
#include "blc/pipeline/record.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace blc {

// Strict integer fields (design §3.2): base 10, no padding, whitespace or suffix; leading zeros
// only for "0" itself. Unsigned fields reject any sign. Every bound is checked explicitly.
bool parse_unsigned_field(std::string_view s, std::uint64_t max, std::uint64_t& out) noexcept;
bool parse_signed_field(std::string_view s, std::int64_t min, std::int64_t max,
                        std::int64_t& out) noexcept;

// sec * 1e9 + nsec with overflow checks; nsec must be in [0, 999 999 999].
bool join_recv_ns(std::int64_t sec, std::int64_t nsec, std::int64_t& out) noexcept;

std::optional<StreamKind> parse_stream_kind(std::string_view s) noexcept;
bool valid_symbol(std::string_view s) noexcept;  // [A-Z0-9]{2,20}

// One market-data row (§3.3) as the record the ring would carry. Views point into the fields.
struct MarketRow {
    RecordHeader header;  // kind Market, symbol index 0, steady_ns 0
    Venue venue = Venue::Spot;
    std::string_view symbol;
    std::string_view payload;
};

// One sidecar row (§3.5).
struct SidecarRow {
    RecordHeader header;  // kind Snapshot, symbol index 0, steady_ns 0
    Venue venue = Venue::Spot;
    std::string_view symbol;
    std::uint64_t apply_before_row = 0;
    std::string_view payload;
};

// Validate and convert a row; nullptr on success, otherwise a description of the first problem.
const char* parse_market_row(const std::vector<std::string_view>& fields, MarketRow& out) noexcept;
const char* parse_sidecar_row(const std::vector<std::string_view>& fields,
                              SidecarRow& out) noexcept;

}  // namespace blc
