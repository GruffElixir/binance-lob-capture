#pragma once

#include "blc/core/types.hpp"

#include <cstdint>

namespace blc {

enum class RecordKind : std::uint8_t { Market, Snapshot };

// Fixed metadata carried with every record from the io thread (or replay) to the processor.
struct RecordHeader {
    RecordKind kind = RecordKind::Market;
    StreamKind stream = StreamKind::DepthDiff;  // Market records only
    std::uint16_t symbol = 0;                   // index into the process-wide symbol list
    std::uint16_t shard = 0;
    std::uint32_t epoch = 0;
    std::uint64_t conn_seq = 0;
    std::int64_t recv_ns = 0;    // wall clock when the message finished arriving
    std::int64_t steady_ns = 0;  // steady clock at the same moment (latency metric); 0 in replay
    std::uint32_t payload_len = 0;
};

// A record as seen by the processor: the payload stays valid until the record is released.
struct RecordView {
    RecordHeader header;
    PaddedView payload{};
};

}  // namespace blc
