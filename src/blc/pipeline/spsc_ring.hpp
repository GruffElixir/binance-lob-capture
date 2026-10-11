#pragma once

#include "blc/pipeline/record.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

namespace blc {

// Single-producer / single-consumer byte ring (design §6.4). Variable-length records are packed
// back to back, each contiguous, 64-byte aligned and followed by simdjson padding so it can be
// parsed in place. Positions are monotonically increasing 64-bit counters over a power-of-two
// buffer. The busy path takes no lock; an idle consumer parks on a condition variable and the
// producer notifies only when it is parked.
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity_pow2);
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // 16 MiB for 1-64 symbols, 32 MiB for 65-128, 64 MiB above.
    static std::size_t capacity_for_symbols(std::size_t n) noexcept;

    // Bytes one record with `payload_len` payload bytes occupies.
    static std::size_t record_bytes(std::size_t payload_len) noexcept;

    // Producer: copies the record in; false (nothing written) if it does not fit right now.
    bool try_push(const RecordHeader& header, std::string_view payload) noexcept;

    // Consumer: the oldest record, valid until pop(); false if the ring is empty.
    bool try_peek(RecordView& out) noexcept;
    void pop() noexcept;

    // Producer side is finished; the consumer drains what remains and then stops.
    void close_producer() noexcept;
    bool producer_closed() const noexcept { return closed_.load(std::memory_order_acquire); }

    // Consumer: waits up to `timeout` for data or closure (spins briefly, then parks).
    void wait(std::chrono::milliseconds timeout) noexcept;

    std::size_t high_water() const noexcept { return high_water_.load(std::memory_order_relaxed); }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    struct AlignedDelete {
        void operator()(unsigned char* p) const noexcept;
    };

    bool empty_for_consumer() noexcept;
    void notify_if_parked() noexcept;

    std::size_t capacity_;
    std::size_t mask_;
    std::unique_ptr<unsigned char[], AlignedDelete> buffer_;

    alignas(64) std::atomic<std::uint64_t> head_{0};  // written by the producer
    std::uint64_t cached_tail_ = 0;                   // producer's copy of tail_
    std::atomic<std::size_t> high_water_{0};

    alignas(64) std::atomic<std::uint64_t> tail_{0};  // written by the consumer
    std::uint64_t cached_head_ = 0;                   // consumer's copy of head_
    std::uint64_t peeked_bytes_ = 0;

    alignas(64) std::atomic<bool> parked_{false};
    std::atomic<bool> closed_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
};

}  // namespace blc
