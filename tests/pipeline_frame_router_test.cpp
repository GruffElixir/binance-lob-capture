#include "blc/core/symbols.hpp"
#include "blc/parse/envelope.hpp"
#include "blc/parse/stream_table.hpp"
#include "blc/pipeline/frame_router.hpp"
#include "blc/pipeline/metrics.hpp"
#include "blc/pipeline/spsc_ring.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

std::string frame(const std::string& stream, const std::string& data) {
    return R"({"stream":")" + stream + R"(","data":)" + data + "}";
}

const std::string kTrade = R"({"e":"trade","s":"BTCUSDT","t":1})";

struct Fixture {
    explicit Fixture(std::size_t ring_bytes = 1u << 16)
        : table(parse_symbol_list(Venue::Spot, "BTCUSDT,ETHUSDT"), 4),
          ring(ring_bytes),
          router(7, table, splitter, ring, metrics) {}

    std::vector<RecordView> drain() {
        std::vector<RecordView> out;
        RecordView v;
        while (ring.try_peek(v)) {
            payloads.emplace_back(v.payload.data, v.payload.size);
            out.push_back(v);
            ring.pop();
        }
        return out;
    }

    StreamTable table;
    EnvelopeSplitter splitter;
    SpscRing ring;
    IoMetrics metrics;
    FrameRouter router;
    std::vector<std::string> payloads;
};

TEST(FrameRouter, AssignsConnSeqAndPushes) {
    Fixture f;
    f.router.on_connected(3);
    EXPECT_EQ(f.router.on_frame(frame("btcusdt@trade", kTrade), 100, 200), FrameResult::Pushed);
    EXPECT_EQ(f.router.on_frame(frame("ethusdt@depth@100ms", kTrade), 101, 201),
              FrameResult::Pushed);
    EXPECT_EQ(f.router.on_frame(frame("btcusdt@depth5@100ms", kTrade), 102, 202),
              FrameResult::Pushed);
    const auto recs = f.drain();
    ASSERT_EQ(recs.size(), 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(recs[i].header.kind, RecordKind::Market);
        EXPECT_EQ(recs[i].header.conn_seq, i);
        EXPECT_EQ(recs[i].header.shard, 7);
        EXPECT_EQ(recs[i].header.epoch, 3u);
        EXPECT_EQ(recs[i].header.recv_ns, static_cast<std::int64_t>(100 + i));
        EXPECT_EQ(recs[i].header.steady_ns, static_cast<std::int64_t>(200 + i));
        EXPECT_EQ(f.payloads[i], kTrade);
    }
    EXPECT_EQ(recs[0].header.symbol, 4);
    EXPECT_EQ(recs[0].header.stream, StreamKind::Trade);
    EXPECT_EQ(recs[1].header.symbol, 5);
    EXPECT_EQ(recs[1].header.stream, StreamKind::DepthDiff);
    EXPECT_EQ(recs[2].header.stream, StreamKind::Depth5);
    EXPECT_EQ(f.metrics.frames_received.load(), 3u);
    EXPECT_EQ(f.metrics.stream_frames[static_cast<std::size_t>(StreamKind::Trade)].load(), 1u);
    EXPECT_EQ(f.metrics.stream_bytes[static_cast<std::size_t>(StreamKind::Trade)].load(),
              frame("btcusdt@trade", kTrade).size());
}

TEST(FrameRouter, UnsubscribedCountedNotLost) {
    Fixture f;
    f.router.on_connected(0);
    EXPECT_EQ(f.router.on_frame(frame("xrpusdt@trade", kTrade), 1, 1), FrameResult::Unsubscribed);
    EXPECT_FALSE(f.router.lost());
    EXPECT_EQ(f.router.on_frame(frame("btcusdt@trade", kTrade), 2, 2), FrameResult::Pushed);
    const auto recs = f.drain();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].header.conn_seq, 1u);  // seq 0 consumed by the unsubscribed frame
    EXPECT_EQ(f.metrics.malformed_frames.load(), 1u);
    EXPECT_EQ(f.metrics.shard_losses.load(), 0u);
    EXPECT_TRUE(f.router.can_deliver_snapshot(0));
}

TEST(FrameRouter, InvalidEnvelopeIsLoss) {
    Fixture f;
    f.router.on_connected(0);
    EXPECT_EQ(f.router.on_frame(R"({"stream":"btcusdt@trade","data":{)", 1, 1),
              FrameResult::LostNow);
    EXPECT_TRUE(f.router.lost());
    EXPECT_EQ(f.router.on_frame(frame("btcusdt@trade", kTrade), 2, 2), FrameResult::Withheld);
    EXPECT_TRUE(f.drain().empty());
    EXPECT_EQ(f.metrics.malformed_frames.load(), 1u);
    EXPECT_EQ(f.metrics.ring_drops.load(), 1u);
    EXPECT_EQ(f.metrics.shard_losses.load(), 1u);
    EXPECT_FALSE(f.router.can_deliver_snapshot(0));
}

TEST(FrameRouter, RingFullIsLoss) {
    Fixture f(1024);
    f.router.on_connected(2);
    const std::string payload = R"({"x":")" + std::string(500, 'a') + R"("})";
    FrameResult r = FrameResult::Pushed;
    int pushed = 0;
    while ((r = f.router.on_frame(frame("btcusdt@trade", payload), 1, 1)) == FrameResult::Pushed) {
        ++pushed;
    }
    EXPECT_EQ(r, FrameResult::LostNow);
    EXPECT_GE(pushed, 1);
    EXPECT_TRUE(f.router.lost());
    EXPECT_FALSE(f.router.can_deliver_snapshot(2));
    EXPECT_EQ(f.metrics.ring_drops.load(), 1u);
    EXPECT_EQ(f.metrics.shard_losses.load(), 1u);
    // Space frees up, but nothing more of this connection is delivered.
    EXPECT_EQ(f.drain().size(), static_cast<std::size_t>(pushed));
    EXPECT_EQ(f.router.on_frame(frame("btcusdt@trade", kTrade), 1, 1), FrameResult::Withheld);
    EXPECT_TRUE(f.drain().empty());
}

TEST(FrameRouter, NoDeliveryAfterDisconnect) {
    Fixture f;
    EXPECT_FALSE(f.router.can_deliver_snapshot(0));  // never connected
    f.router.on_connected(0);
    EXPECT_TRUE(f.router.can_deliver_snapshot(0));
    f.router.on_disconnected();
    EXPECT_FALSE(f.router.can_deliver_snapshot(0));
    f.router.on_connected(1);
    EXPECT_TRUE(f.router.can_deliver_snapshot(1));
    EXPECT_FALSE(f.router.can_deliver_snapshot(0));
}

TEST(FrameRouter, NewConnectionClearsLoss) {
    Fixture f;
    f.router.on_connected(0);
    f.router.on_frame(frame("btcusdt@trade", kTrade), 1, 1);
    f.router.on_frame("garbage", 1, 1);
    ASSERT_TRUE(f.router.lost());
    f.router.on_disconnected();
    f.drain();
    f.router.on_connected(1);
    EXPECT_FALSE(f.router.lost());
    EXPECT_EQ(f.router.on_frame(frame("btcusdt@trade", kTrade), 5, 5), FrameResult::Pushed);
    const auto recs = f.drain();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].header.conn_seq, 0u);
    EXPECT_EQ(recs[0].header.epoch, 1u);
}

TEST(FrameRouter, SnapshotSteadyTimeSurvivesRingRetry) {
    Fixture f(1024);
    f.router.on_connected(4);
    const std::string body =
        R"({"lastUpdateId":1,"bids":[],"asks":[],"pad":")" + std::string(500, 'p') + R"("})";
    ASSERT_TRUE(f.router.push_snapshot(1, 4, body, 1000, 2000));
    EXPECT_FALSE(f.router.push_snapshot(0, 4, body, 1111, 2222));  // ring full
    EXPECT_EQ(f.drain().size(), 1u);
    f.payloads.clear();
    ASSERT_TRUE(f.router.can_deliver_snapshot(4));
    ASSERT_TRUE(f.router.push_snapshot(0, 4, body, 1111, 2222));
    const auto recs = f.drain();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].header.kind, RecordKind::Snapshot);
    EXPECT_EQ(recs[0].header.symbol, 0);
    EXPECT_EQ(recs[0].header.shard, 7);
    EXPECT_EQ(recs[0].header.epoch, 4u);
    EXPECT_EQ(recs[0].header.recv_ns, 1111);
    EXPECT_EQ(recs[0].header.steady_ns, 2222);
    EXPECT_EQ(f.payloads[0], body);
    EXPECT_EQ(f.metrics.frames_received.load(), 0u);  // snapshots are not frames
    EXPECT_EQ(f.metrics.ring_drops.load(), 0u);       // a snapshot retry is not a drop
    EXPECT_FALSE(f.router.lost());
}

TEST(FrameRouter, AccountingInvariant) {
    Fixture f(4096);
    f.router.on_connected(9);
    std::uint64_t pushed = 0;
    const std::string payload = R"({"x":")" + std::string(300, 'a') + R"("})";
    for (int i = 0; i < 40; ++i) {
        const FrameResult r = i == 3 ? f.router.on_frame(frame("dogeusdt@trade", kTrade), 1, 1)
                                     : f.router.on_frame(frame("btcusdt@trade", payload), 1, 1);
        if (r == FrameResult::Pushed) {
            ++pushed;
        }
        if (i < 12 && i % 3 == 0) {
            f.drain();
        }
    }
    ASSERT_TRUE(f.router.lost());
    const EpochStats s = f.router.close_epoch();
    EXPECT_EQ(s.frames, 40u);
    EXPECT_EQ(s.malformed, 1u);
    EXPECT_EQ(s.frames, pushed + s.malformed + s.drops);
    EXPECT_EQ(f.metrics.frames_received.load(),
              pushed + f.metrics.malformed_frames.load() + f.metrics.ring_drops.load());
    // The next epoch starts from zero.
    f.drain();
    f.router.on_connected(10);
    f.router.on_frame(frame("btcusdt@trade", kTrade), 1, 1);
    const EpochStats next = f.router.close_epoch();
    EXPECT_EQ(next.frames, 1u);
    EXPECT_EQ(next.malformed, 0u);
    EXPECT_EQ(next.drops, 0u);
}

TEST(FrameRouter, ReversedEnvelopeSameRecord) {
    Fixture forward;
    Fixture reversed;
    forward.router.on_connected(1);
    reversed.router.on_connected(1);
    ASSERT_EQ(forward.router.on_frame(frame("ethusdt@trade", kTrade), 50, 60), FrameResult::Pushed);
    ASSERT_EQ(
        reversed.router.on_frame(R"({"data":)" + kTrade + R"(,"stream":"ethusdt@trade"})", 50, 60),
        FrameResult::Pushed);
    const auto a = forward.drain();
    const auto b = reversed.drain();
    ASSERT_EQ(a.size(), 1u);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(a[0].header.kind, b[0].header.kind);
    EXPECT_EQ(a[0].header.stream, b[0].header.stream);
    EXPECT_EQ(a[0].header.symbol, b[0].header.symbol);
    EXPECT_EQ(a[0].header.shard, b[0].header.shard);
    EXPECT_EQ(a[0].header.epoch, b[0].header.epoch);
    EXPECT_EQ(a[0].header.conn_seq, b[0].header.conn_seq);
    EXPECT_EQ(a[0].header.recv_ns, b[0].header.recv_ns);
    EXPECT_EQ(a[0].header.steady_ns, b[0].header.steady_ns);
    EXPECT_EQ(a[0].header.payload_len, b[0].header.payload_len);
    EXPECT_EQ(forward.payloads[0], reversed.payloads[0]);
    EXPECT_EQ(reversed.splitter.fallbacks(), 1u);
}

}  // namespace
}  // namespace blc
