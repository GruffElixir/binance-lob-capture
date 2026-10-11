#include "blc/csv/rows.hpp"

#include "blc/csv/schema.hpp"

#include <limits>

namespace blc {

namespace {

bool digits_only(std::string_view s) noexcept {
    if (s.empty() || (s.size() > 1 && s[0] == '0')) {
        return false;
    }
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

bool payload_one_line(std::string_view p) noexcept {
    return !p.empty() && p.find_first_of("\r\n") == std::string_view::npos;
}

// Fields shared by both row kinds: recv_tsec, recv_tnsec, shard, epoch.
const char* parse_common(std::string_view tsec, std::string_view tnsec, std::string_view shard,
                         std::string_view epoch, RecordHeader& h) noexcept {
    std::int64_t sec = 0;
    if (!parse_signed_field(tsec, std::numeric_limits<std::int64_t>::min(),
                            std::numeric_limits<std::int64_t>::max(), sec)) {
        return "recv_tsec is not an int64";
    }
    std::int64_t nsec = 0;
    if (!parse_signed_field(tnsec, 0, 999'999'999, nsec)) {
        return "recv_tnsec is not in [0, 999999999]";
    }
    if (!join_recv_ns(sec, nsec, h.recv_ns)) {
        return "receive time overflows int64 nanoseconds";
    }
    std::uint64_t v = 0;
    if (!parse_unsigned_field(shard, std::numeric_limits<std::uint16_t>::max(), v)) {
        return "shard_id is not a uint16";
    }
    h.shard = static_cast<std::uint16_t>(v);
    if (!parse_unsigned_field(epoch, std::numeric_limits<std::uint32_t>::max(), v)) {
        return "conn_epoch is not a uint32";
    }
    h.epoch = static_cast<std::uint32_t>(v);
    h.steady_ns = 0;
    h.symbol = 0;
    return nullptr;
}

}  // namespace

bool parse_unsigned_field(std::string_view s, std::uint64_t max, std::uint64_t& out) noexcept {
    if (!digits_only(s)) {
        return false;
    }
    std::uint64_t v = 0;
    for (const char c : s) {
        if (__builtin_mul_overflow(v, std::uint64_t{10}, &v) ||
            __builtin_add_overflow(v, static_cast<std::uint64_t>(c - '0'), &v)) {
            return false;
        }
    }
    if (v > max) {
        return false;
    }
    out = v;
    return true;
}

bool parse_signed_field(std::string_view s, std::int64_t min, std::int64_t max,
                        std::int64_t& out) noexcept {
    const bool negative = !s.empty() && s[0] == '-';
    if (negative) {
        s.remove_prefix(1);
        if (s == "0") {
            return false;  // "-0" is not canonical
        }
    }
    if (!digits_only(s)) {
        return false;
    }
    std::int64_t v = 0;
    for (const char c : s) {
        const std::int64_t d = c - '0';
        if (__builtin_mul_overflow(v, std::int64_t{10}, &v) ||
            (negative ? __builtin_sub_overflow(v, d, &v) : __builtin_add_overflow(v, d, &v))) {
            return false;
        }
    }
    if (v < min || v > max) {
        return false;
    }
    out = v;
    return true;
}

bool join_recv_ns(std::int64_t sec, std::int64_t nsec, std::int64_t& out) noexcept {
    if (nsec < 0 || nsec > 999'999'999) {
        return false;
    }
    std::int64_t ns = 0;
    if (__builtin_mul_overflow(sec, std::int64_t{1'000'000'000}, &ns) ||
        __builtin_add_overflow(ns, nsec, &ns)) {
        return false;
    }
    out = ns;
    return true;
}

std::optional<StreamKind> parse_stream_kind(std::string_view s) noexcept {
    for (const StreamKind k : {StreamKind::DepthDiff, StreamKind::Depth5, StreamKind::Trade}) {
        if (s == to_string(k)) {
            return k;
        }
    }
    return std::nullopt;
}

bool valid_symbol(std::string_view s) noexcept {
    if (s.size() < 2 || s.size() > 20) {
        return false;
    }
    for (const char c : s) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

const char* parse_market_row(const std::vector<std::string_view>& f, MarketRow& out) noexcept {
    if (f.size() != kMarketDataColumns) {
        return "wrong number of columns";
    }
    out.header = RecordHeader{};
    out.header.kind = RecordKind::Market;
    if (const char* e = parse_common(f[0], f[1], f[4], f[5], out.header)) {
        return e;
    }
    const auto venue = parse_venue(f[2]);
    if (!venue) {
        return "venue is not spot or usdm";
    }
    out.venue = *venue;
    const auto kind = parse_stream_kind(f[3]);
    if (!kind) {
        return "stream_kind is not depth_diff, depth5 or trade";
    }
    out.header.stream = *kind;
    std::uint64_t seq = 0;
    if (!parse_unsigned_field(f[6], std::numeric_limits<std::uint64_t>::max(), seq)) {
        return "conn_seq is not a uint64";
    }
    out.header.conn_seq = seq;
    if (!valid_symbol(f[7])) {
        return "symbol is not [A-Z0-9]{2,20}";
    }
    out.symbol = f[7];
    if (!payload_one_line(f[8])) {
        return "payload_json is empty or spans lines";
    }
    out.payload = f[8];
    out.header.payload_len = static_cast<std::uint32_t>(f[8].size());
    return nullptr;
}

const char* parse_sidecar_row(const std::vector<std::string_view>& f, SidecarRow& out) noexcept {
    if (f.size() != kSnapshotColumns) {
        return "wrong number of columns";
    }
    out.header = RecordHeader{};
    out.header.kind = RecordKind::Snapshot;
    if (const char* e = parse_common(f[0], f[1], f[3], f[4], out.header)) {
        return e;
    }
    const auto venue = parse_venue(f[2]);
    if (!venue) {
        return "venue is not spot or usdm";
    }
    out.venue = *venue;
    if (!valid_symbol(f[5])) {
        return "symbol is not [A-Z0-9]{2,20}";
    }
    out.symbol = f[5];
    if (!parse_unsigned_field(f[6], std::numeric_limits<std::size_t>::max(),
                              out.apply_before_row)) {
        return "apply_before_row is not a size_t";
    }
    if (!payload_one_line(f[7])) {
        return "payload_json is empty or spans lines";
    }
    out.payload = f[7];
    out.header.payload_len = static_cast<std::uint32_t>(f[7].size());
    return nullptr;
}

}  // namespace blc
