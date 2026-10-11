#pragma once

#include "blc/parse/envelope.hpp"
#include "blc/parse/stream_table.hpp"
#include "blc/pipeline/metrics.hpp"
#include "blc/pipeline/spsc_ring.hpp"

#include <cstdint>
#include <string_view>

namespace blc {

enum class FrameResult : std::uint8_t {
    Pushed,        // routed and pushed to the ring
    Unsubscribed,  // valid envelope for a stream this shard does not carry: counted only
    LostNow,       // this frame was lost (invalid envelope or full ring): the shard is now lost
    Withheld,      // discarded because the shard is already lost on this connection
};

// Socket-independent per-shard routing (design §6.2): assigns conn_seq, splits the envelope,
// routes the stream name and pushes the record. Owns the loss transition: after a frame is lost,
// every further frame of the connection is withheld and no snapshot is delivered, until the next
// connection starts a new epoch. Used on the io thread only.
class FrameRouter {
public:
    FrameRouter(std::uint16_t shard, const StreamTable& table, EnvelopeSplitter& shared,
                SpscRing& ring, IoMetrics& metrics) noexcept;

    void on_connected(std::uint32_t epoch) noexcept;
    FrameResult on_frame(std::string_view frame, std::int64_t recv_ns,
                         std::int64_t steady_ns) noexcept;
    bool lost() const noexcept { return lost_; }
    std::uint16_t shard_id() const noexcept { return shard_; }

    // Any read error, close or loss: the connection is no longer established.
    void on_disconnected() noexcept { established_ = false; }

    // A snapshot for `epoch` may be delivered only on the current, established, loss-free
    // connection (design §6.3).
    bool can_deliver_snapshot(std::uint32_t epoch) const noexcept {
        return established_ && !lost_ && epoch == epoch_;
    }

    // Pushes a snapshot record; false if the ring is full (the caller retries with the same
    // timestamps). The caller checks can_deliver_snapshot() before every attempt.
    bool push_snapshot(std::uint16_t symbol, std::uint32_t epoch, std::string_view body,
                       std::int64_t recv_ns, std::int64_t steady_ns) noexcept;

    // Returns the accounting of the current epoch and starts a fresh one. The caller logs the
    // epoch_closed line.
    EpochStats close_epoch() noexcept;

private:
    FrameResult lose(bool malformed) noexcept;

    std::uint16_t shard_;
    const StreamTable& table_;
    EnvelopeSplitter& splitter_;
    SpscRing& ring_;
    IoMetrics& metrics_;

    std::uint32_t epoch_ = 0;
    std::uint64_t next_seq_ = 0;
    bool established_ = false;
    bool lost_ = false;
    EpochStats stats_;
};

}  // namespace blc
