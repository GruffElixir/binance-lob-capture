#pragma once

#include <cstddef>
#include <string_view>

namespace blc {

// The only place the exact CSV header strings exist (design §3.3, §3.4, §3.5).

inline constexpr std::string_view kMarketDataHeader =
    "recv_tsec,recv_tnsec,venue,stream_kind,shard_id,conn_epoch,conn_seq,symbol,payload_json";

inline constexpr std::string_view kOrderBookHeader =
    "tsec,tnsec,seqNo,id,type,side,"
    "bid0,bid1,bid2,bid3,bid4,"
    "bid_size0,bid_size1,bid_size2,bid_size3,bid_size4,"
    "ask0,ask1,ask2,ask3,ask4,"
    "ask_size0,ask_size1,ask_size2,ask_size3,ask_size4";

inline constexpr std::string_view kSnapshotHeader =
    "recv_tsec,recv_tnsec,venue,shard_id,conn_epoch,symbol,apply_before_row,payload_json";

constexpr std::size_t column_count(std::string_view header) noexcept {
    std::size_t n = 1;
    for (const char c : header) {
        if (c == ',') {
            ++n;
        }
    }
    return n;
}

inline constexpr std::size_t kMarketDataColumns = 9;
inline constexpr std::size_t kOrderBookColumns = 26;
inline constexpr std::size_t kSnapshotColumns = 8;

static_assert(column_count(kMarketDataHeader) == kMarketDataColumns);
static_assert(column_count(kOrderBookHeader) == kOrderBookColumns);
static_assert(column_count(kSnapshotHeader) == kSnapshotColumns);

}  // namespace blc
