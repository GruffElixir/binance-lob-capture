#pragma once

#include "blc/core/types.hpp"
#include "blc/parse/events.hpp"
#include "blc/pipeline/record.hpp"

#include <cstdint>
#include <simdjson.h>
#include <string>
#include <string_view>
#include <vector>

namespace blc::test {

// Scaled integer -> Binance decimal text with 8 decimals ("100.00000000").
std::string e8(std::int64_t scaled);

// Binance-shaped payloads for typed events (the inverse of PayloadParser).
std::string diff_json(Venue venue, std::string_view symbol, const DepthDiff& diff);
std::string depth5_json(Venue venue, std::string_view symbol, const Depth5& d5);
std::string trade_json(Venue venue, std::string_view symbol, bool buyer_is_maker,
                       std::uint64_t trade_id = 1);
std::string snapshot_json(Venue venue, const DepthSnapshot& snapshot);

DepthDiff make_diff(std::uint64_t U, std::uint64_t u, std::uint64_t pu, std::vector<Level> bids,
                    std::vector<Level> asks);
Depth5 make_depth5(std::uint64_t id, const std::vector<Level>& bids,
                   const std::vector<Level>& asks);

// A record with its own padded payload copy.
class OwnedRecord {
public:
    OwnedRecord(const RecordHeader& header, const std::string& payload);

    RecordView view() const;
    const RecordHeader& header() const { return header_; }
    std::string_view payload() const { return std::string_view(bytes_.data(), size_); }

private:
    RecordHeader header_;
    std::vector<char> bytes_;
    std::size_t size_;
};

OwnedRecord market_record(StreamKind kind, std::uint16_t symbol, std::uint32_t epoch,
                          std::uint64_t conn_seq, std::int64_t recv_ns, const std::string& payload,
                          std::uint16_t shard = 0);
OwnedRecord snapshot_record(std::uint16_t symbol, std::uint32_t epoch, std::int64_t recv_ns,
                            const std::string& payload, std::uint16_t shard = 0);

}  // namespace blc::test
