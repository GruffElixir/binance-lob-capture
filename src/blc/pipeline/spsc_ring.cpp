#include "blc/pipeline/spsc_ring.hpp"

#include <cstring>
#include <new>
#include <simdjson.h>
#include <stdexcept>
#include <thread>

namespace blc {
namespace {

constexpr std::size_t kAlign = 64;
constexpr std::size_t kHeaderBytes = 64;  // stored header area; payload starts 64-aligned
constexpr std::uint32_t kWrapMarker = 1;

struct StoredHeader {
    std::uint32_t total_bytes;  // this record (or wrap gap) including header and padding
    std::uint32_t flags;        // kWrapMarker: skip to the start of the buffer
    RecordHeader header;
};
static_assert(sizeof(StoredHeader) <= kHeaderBytes, "stored header must fit its area");

constexpr std::size_t align_up(std::size_t n) noexcept {
    return (n + kAlign - 1) & ~(kAlign - 1);
}

}  // namespace

void SpscRing::AlignedDelete::operator()(unsigned char* p) const noexcept {
    ::operator delete[](p, std::align_val_t{kAlign});
}

SpscRing::SpscRing(std::size_t capacity_pow2)
    : capacity_(capacity_pow2),
      mask_(capacity_pow2 - 1),
      buffer_(
          static_cast<unsigned char*>(::operator new[](capacity_pow2, std::align_val_t{kAlign}))) {
    if (capacity_pow2 < 4 * kAlign || (capacity_pow2 & (capacity_pow2 - 1)) != 0) {
        throw std::invalid_argument("SpscRing capacity must be a power of two >= 256");
    }
    std::memset(buffer_.get(), 0, capacity_);
}

std::size_t SpscRing::capacity_for_symbols(std::size_t n) noexcept {
    if (n <= 64) {
        return std::size_t{16} << 20;
    }
    if (n <= 128) {
        return std::size_t{32} << 20;
    }
    return std::size_t{64} << 20;
}

std::size_t SpscRing::record_bytes(std::size_t payload_len) noexcept {
    return align_up(kHeaderBytes + payload_len + simdjson::SIMDJSON_PADDING);
}

bool SpscRing::try_push(const RecordHeader& header, std::string_view payload) noexcept {
    const std::size_t need = record_bytes(payload.size());
    if (need > capacity_) {
        return false;
    }
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::size_t index = static_cast<std::size_t>(head) & mask_;
    const std::size_t to_end = capacity_ - index;
    const bool wrap = need > to_end;
    const std::size_t total = wrap ? to_end + need : need;
    if (total > capacity_) {
        return false;
    }
    if (head + total - cached_tail_ > capacity_) {
        cached_tail_ = tail_.load(std::memory_order_acquire);
        if (head + total - cached_tail_ > capacity_) {
            return false;
        }
    }
    std::size_t at = index;
    if (wrap) {
        StoredHeader marker{};
        marker.total_bytes = static_cast<std::uint32_t>(to_end);
        marker.flags = kWrapMarker;
        std::memcpy(buffer_.get() + index, &marker, sizeof marker);
        at = 0;
    }
    StoredHeader stored{};
    stored.total_bytes = static_cast<std::uint32_t>(need);
    stored.flags = 0;
    stored.header = header;
    stored.header.payload_len = static_cast<std::uint32_t>(payload.size());
    std::memcpy(buffer_.get() + at, &stored, sizeof stored);
    if (!payload.empty()) {
        std::memcpy(buffer_.get() + at + kHeaderBytes, payload.data(), payload.size());
    }
    const std::uint64_t new_head = head + total;
    head_.store(new_head, std::memory_order_release);

    const std::size_t used = static_cast<std::size_t>(new_head - cached_tail_);
    if (used > high_water_.load(std::memory_order_relaxed)) {
        high_water_.store(used, std::memory_order_relaxed);
    }
    notify_if_parked();
    return true;
}

bool SpscRing::try_peek(RecordView& out) noexcept {
    for (;;) {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail == cached_head_) {
                return false;
            }
        }
        const std::size_t index = static_cast<std::size_t>(tail) & mask_;
        StoredHeader stored;
        std::memcpy(&stored, buffer_.get() + index, sizeof stored);
        if ((stored.flags & kWrapMarker) != 0) {
            tail_.store(tail + stored.total_bytes, std::memory_order_release);
            continue;
        }
        out.header = stored.header;
        out.payload =
            PaddedView{reinterpret_cast<const char*>(buffer_.get() + index + kHeaderBytes),
                       stored.header.payload_len, stored.total_bytes - kHeaderBytes};
        peeked_bytes_ = stored.total_bytes;
        return true;
    }
}

void SpscRing::pop() noexcept {
    tail_.store(tail_.load(std::memory_order_relaxed) + peeked_bytes_, std::memory_order_release);
    peeked_bytes_ = 0;
}

void SpscRing::close_producer() noexcept {
    closed_.store(true, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const std::lock_guard<std::mutex> lock(mutex_);
    cv_.notify_all();
}

bool SpscRing::empty_for_consumer() noexcept {
    return tail_.load(std::memory_order_relaxed) == head_.load(std::memory_order_acquire);
}

void SpscRing::notify_if_parked() noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (parked_.load(std::memory_order_relaxed)) {
        const std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_one();
    }
}

void SpscRing::wait(std::chrono::milliseconds timeout) noexcept {
    for (int i = 0; i < 256; ++i) {
        if (!empty_for_consumer() || producer_closed()) {
            return;
        }
        if (i >= 64) {
            std::this_thread::yield();
        }
    }
    std::unique_lock<std::mutex> lock(mutex_);
    parked_.store(true, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (empty_for_consumer() && !producer_closed()) {
        cv_.wait_for(lock, timeout);
    }
    parked_.store(false, std::memory_order_relaxed);
}

}  // namespace blc
