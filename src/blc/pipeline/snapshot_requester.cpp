#include "blc/pipeline/snapshot_requester.hpp"

namespace blc {

RequestSlots::RequestSlots(std::size_t symbols)
    : size_(symbols),
      slots_(std::make_unique<std::atomic<std::uint64_t>[]>(symbols)),
      numbers_(symbols, 0),
      seen_(symbols, 0) {
    for (std::size_t i = 0; i < symbols; ++i) {
        slots_[i].store(0, std::memory_order_relaxed);
    }
}

void RequestSlots::request(std::uint16_t symbol, std::uint32_t epoch) noexcept {
    const std::uint32_t number = ++numbers_[symbol];
    slots_[symbol].store((std::uint64_t{epoch} << 32) | number, std::memory_order_release);
}

}  // namespace blc
