#include "blc/pipeline/frame_router.hpp"

namespace blc {

FrameRouter::FrameRouter(std::uint16_t shard, const StreamTable& table, EnvelopeSplitter& shared,
                         SpscRing& ring, IoMetrics& metrics) noexcept
    : shard_(shard), table_(table), splitter_(shared), ring_(ring), metrics_(metrics) {}

void FrameRouter::on_connected(std::uint32_t epoch) noexcept {
    epoch_ = epoch;
    next_seq_ = 0;
    established_ = true;
    lost_ = false;
}

FrameResult FrameRouter::lose(bool malformed) noexcept {
    if (malformed) {
        IoMetrics::add(metrics_.malformed_frames);
        ++stats_.malformed;
    } else {
        IoMetrics::add(metrics_.ring_drops);
        ++stats_.drops;
    }
    IoMetrics::add(metrics_.shard_losses);
    lost_ = true;
    return FrameResult::LostNow;
}

FrameResult FrameRouter::on_frame(std::string_view frame, std::int64_t recv_ns,
                                  std::int64_t steady_ns) noexcept {
    const std::uint64_t seq = next_seq_++;
    IoMetrics::add(metrics_.frames_received);
    IoMetrics::add(metrics_.bytes_received, frame.size());
    ++stats_.frames;
    if (lost_) {
        IoMetrics::add(metrics_.ring_drops);
        ++stats_.drops;
        return FrameResult::Withheld;
    }
    Envelope env;
    if (!splitter_.split(frame, env)) {
        return lose(true);
    }
    const auto entry = table_.find(env.stream);
    if (!entry) {
        IoMetrics::add(metrics_.malformed_frames);
        ++stats_.malformed;
        return FrameResult::Unsubscribed;
    }
    const auto kind = static_cast<std::size_t>(entry->kind);
    IoMetrics::add(metrics_.stream_frames[kind]);
    IoMetrics::add(metrics_.stream_bytes[kind], frame.size());

    RecordHeader h;
    h.kind = RecordKind::Market;
    h.stream = entry->kind;
    h.symbol = entry->symbol_index;
    h.shard = shard_;
    h.epoch = epoch_;
    h.conn_seq = seq;
    h.recv_ns = recv_ns;
    h.steady_ns = steady_ns;
    if (!ring_.try_push(h, env.data)) {
        return lose(false);
    }
    return FrameResult::Pushed;
}

bool FrameRouter::push_snapshot(std::uint16_t symbol, std::uint32_t epoch, std::string_view body,
                                std::int64_t recv_ns, std::int64_t steady_ns) noexcept {
    RecordHeader h;
    h.kind = RecordKind::Snapshot;
    h.symbol = symbol;
    h.shard = shard_;
    h.epoch = epoch;
    h.recv_ns = recv_ns;
    h.steady_ns = steady_ns;
    return ring_.try_push(h, body);
}

EpochStats FrameRouter::close_epoch() noexcept {
    const EpochStats s = stats_;
    stats_ = EpochStats{};
    return s;
}

}  // namespace blc
