#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace blc {

// How the processor asks for a REST snapshot (design §6.1). Never blocks or allocates.
class SnapshotRequester {
public:
    virtual ~SnapshotRequester() = default;
    virtual void request(std::uint16_t symbol, std::uint32_t epoch) noexcept = 0;
};

// One atomic slot per symbol holding (epoch << 32) | request_number; request numbers start at 1
// and only the latest request is kept, so a lagging processor can never overflow it. The io
// thread scans the slots on a timer and hands every changed slot to the REST scheduler.
class RequestSlots final : public SnapshotRequester {
public:
    explicit RequestSlots(std::size_t symbols);

    // Processor thread.
    void request(std::uint16_t symbol, std::uint32_t epoch) noexcept override;

    // io thread: calls on_changed(symbol, epoch, request_number) for every slot that changed
    // since the previous scan.
    template <class F>
    void scan(F&& on_changed) {
        for (std::size_t i = 0; i < size_; ++i) {
            const std::uint64_t v = slots_[i].load(std::memory_order_acquire);
            if (v != seen_[i]) {
                seen_[i] = v;
                on_changed(i, static_cast<std::uint32_t>(v >> 32),
                           static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
            }
        }
    }

    std::uint64_t value(std::size_t symbol) const noexcept {
        return slots_[symbol].load(std::memory_order_acquire);
    }
    std::size_t size() const noexcept { return size_; }

private:
    std::size_t size_;
    // NOLINTNEXTLINE(modernize-avoid-c-arrays): dynamically sized atomic slots, reserved once
    std::unique_ptr<std::atomic<std::uint64_t>[]>
        slots_;  // NOLINT(modernize-avoid-c-arrays): dynamically sized preallocated backing storage
    std::vector<std::uint32_t> numbers_;  // processor side
    std::vector<std::uint64_t> seen_;     // io side
};

// Replay: snapshots come from the sidecar, so requests go nowhere.
class NullRequester final : public SnapshotRequester {
public:
    void request(std::uint16_t /*symbol*/, std::uint32_t /*epoch*/) noexcept override {}
};

}  // namespace blc
