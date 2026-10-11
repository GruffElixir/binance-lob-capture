#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace blc {

inline constexpr std::size_t kStreamKindCount = 3;

// io-thread counters (design §8). Written by the io thread only, read by the processor for the
// stats line; relaxed ordering is enough because each value is independent.
struct IoMetrics {
    using Counter = std::atomic<std::uint64_t>;

    Counter frames_received{0};
    Counter bytes_received{0};
    Counter malformed_frames{0};  // invalid envelope or unsubscribed stream
    Counter ring_drops{0};        // dropped on a full ring or withheld after a loss
    Counter shard_losses{0};
    Counter snapshots_discarded{0};
    Counter connects{0};
    Counter reconnects{0};
    Counter connect_failures{0};
    Counter connect_throttled{0};
    Counter rest_requests{0};
    Counter rest_failures{0};
    Counter rest_cooldowns{0};
    Counter rest_weight_used{0};
    std::array<Counter, kStreamKindCount> stream_frames{};  // indexed by StreamKind
    std::array<Counter, kStreamKindCount> stream_bytes{};
    Counter receive_capacity_bytes{0};  // sum of all shard receive-buffer capacities

    static void add(Counter& c, std::uint64_t n = 1) noexcept {
        c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
};

// Frame accounting of one (shard, epoch): frames == pushed + malformed + drops (design §8).
struct EpochStats {
    std::uint64_t frames = 0;
    std::uint64_t malformed = 0;
    std::uint64_t drops = 0;
};

}  // namespace blc
