#include "support/loss_scenario.hpp"

#include "blc/core/symbols.hpp"
#include "blc/parse/envelope.hpp"
#include "blc/parse/stream_table.hpp"
#include "blc/pipeline/frame_router.hpp"
#include "blc/pipeline/metrics.hpp"
#include "blc/pipeline/output_files.hpp"
#include "blc/pipeline/processor.hpp"
#include "blc/pipeline/snapshot_requester.hpp"
#include "blc/pipeline/spsc_ring.hpp"
#include "support/payload_json.hpp"

#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace blc::test {

namespace {

constexpr std::int64_t P = kPriceScale;
constexpr std::size_t kRingBytes = 4096;

// Mirrors SpscRing's documented placement (contiguous records, wrap marker before a record that
// does not fit before the end) so the fill stops exactly when the diff no longer fits. Every
// push result is checked against it, so the model cannot silently drift from the ring.
struct RingModel {
    std::uint64_t head = 0;
    std::uint64_t tail = 0;

    std::size_t total(std::size_t payload_len) const {
        const std::size_t need = SpscRing::record_bytes(payload_len);
        const std::size_t index = static_cast<std::size_t>(head % kRingBytes);
        return need > kRingBytes - index ? kRingBytes - index + need : need;
    }
    bool fits(std::size_t payload_len) const {
        return head + total(payload_len) - tail <= kRingBytes;
    }
    void push(std::size_t payload_len) { head += total(payload_len); }
    void drained() { tail = head; }
};

std::string frame(const std::string& stream, const std::string& data) {
    return R"({"stream":")" + stream + R"(","data":)" + data + "}";
}

}  // namespace

void write_loss_scenario(const std::filesystem::path& dir) {
    const auto symbols = parse_symbol_list(Venue::Spot, "BTCUSDT");
    const StreamTable table(symbols, 0);
    EnvelopeSplitter splitter;
    SpscRing ring(kRingBytes);
    IoMetrics metrics;
    FrameRouter router(0, table, splitter, ring, metrics);

    auto scratch = std::make_unique<char[]>(SymbolFiles::kBufferBytes);
    std::vector<std::unique_ptr<SymbolOutput>> outputs;
    outputs.push_back(std::make_unique<FileOutput>(
        std::make_unique<SymbolFiles>(dir, file_names(Venue::Spot, "BTCUSDT", "2026-10-11"),
                                      scratch.get(), SymbolFiles::kBufferBytes),
        Venue::Spot, "BTCUSDT"));
    RequestSlots slots(1);
    Processor processor(ProcessorConfig{Venue::Spot, symbols, true}, std::move(outputs), slots);

    RingModel model;
    const auto drain = [&] {
        RecordView v;
        while (ring.try_peek(v)) {
            processor.process(v);
            ring.pop();
        }
        model.drained();
    };
    std::int64_t step = 0;
    const auto at = [&](std::int64_t s) { return kLossT0 + s * kLossStepNs; };
    const auto send = [&](const std::string& stream, const std::string& data) {
        const bool expect_fit = model.fits(data.size());
        const FrameResult r = router.on_frame(frame(stream, data), at(step++), 0);
        if (r == FrameResult::Pushed) {
            EXPECT_TRUE(expect_fit);
            model.push(data.size());
        } else if (r == FrameResult::LostNow) {
            EXPECT_FALSE(expect_fit);
        }
        return r;
    };
    const auto deliver = [&](std::uint32_t epoch, const DepthSnapshot& s) {
        EXPECT_TRUE(router.can_deliver_snapshot(epoch));
        const std::string body = snapshot_json(Venue::Spot, s);
        EXPECT_TRUE(router.push_snapshot(0, epoch, body, at(step++), 0));
        model.push(body.size());
        drain();
    };

    // --- Epoch 0: sync with best bid 100.
    router.on_connected(0);
    EXPECT_EQ(
        send("btcusdt@depth@100ms",
             diff_json(Venue::Spot, "BTCUSDT", make_diff(101, 101, 0, {{100 * P, 2 * P}}, {}))),
        FrameResult::Pushed);
    drain();
    EXPECT_EQ(slots.value(0), 1u);
    DepthSnapshot s0;
    s0.last_update_id = 100;
    s0.bids = {{100 * P, P}, {99 * P, P}, {98 * P, P}};
    s0.asks = {{101 * P, P}, {102 * P, P}};
    deliver(0, s0);
    EXPECT_EQ(send("btcusdt@trade", trade_json(Venue::Spot, "BTCUSDT", true, 1)),
              FrameResult::Pushed);
    drain();

    // Fill the ring (the processor is stalled) until the next diff cannot fit.
    const std::string deleting =
        diff_json(Venue::Spot, "BTCUSDT", make_diff(102, 102, 0, {{100 * P, 0}, {97 * P, P}}, {}));
    std::uint64_t filler_id = 2;
    while (model.fits(deleting.size())) {
        EXPECT_EQ(send("btcusdt@trade", trade_json(Venue::Spot, "BTCUSDT", false, filler_id++)),
                  FrameResult::Pushed);
    }
    EXPECT_LT(step, 1000);
    step = 1000;  // the dropped diff's receive time is kLossDropRecvNs
    EXPECT_EQ(send("btcusdt@depth@100ms", deleting), FrameResult::LostNow);
    EXPECT_EQ(send("btcusdt@trade", trade_json(Venue::Spot, "BTCUSDT", true, kLossWithheldTradeId)),
              FrameResult::Withheld);
    EXPECT_FALSE(router.can_deliver_snapshot(0));
    drain();  // the processor catches up with the frames received before the loss
    router.on_disconnected();
    const EpochStats closed = router.close_epoch();
    EXPECT_EQ(closed.frames, filler_id + 2);  // diff, trade 1, fillers, lost diff, withheld trade
    EXPECT_EQ(closed.drops, 2u);
    EXPECT_EQ(closed.malformed, 0u);

    // --- Epoch 1: reconnect; the symbol resets at its first record and resyncs.
    router.on_connected(1);
    EXPECT_EQ(
        send("btcusdt@depth@100ms",
             diff_json(Venue::Spot, "BTCUSDT", make_diff(103, 103, 0, {}, {{102 * P, 3 * P}}))),
        FrameResult::Pushed);
    drain();
    EXPECT_EQ(slots.value(0), (std::uint64_t{1} << 32) | 2u);
    DepthSnapshot s1;
    s1.last_update_id = 102;
    s1.bids = {{99 * P, P}, {98 * P, P}, {97 * P, P}};
    s1.asks = {{101 * P, P}, {102 * P, P}};
    deliver(1, s1);
    EXPECT_EQ(send("btcusdt@trade", trade_json(Venue::Spot, "BTCUSDT", true, 1000)),
              FrameResult::Pushed);
    drain();
    EXPECT_EQ(processor.counters(0).resets, 2u);
    EXPECT_EQ(processor.counters(0).syncs, 2u);
    EXPECT_TRUE(processor.finish());
}

}  // namespace blc::test
