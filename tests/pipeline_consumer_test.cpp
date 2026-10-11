#include "blc/core/symbols.hpp"
#include "blc/pipeline/consumer.hpp"
#include "blc/pipeline/processor.hpp"
#include "blc/pipeline/snapshot_requester.hpp"
#include "blc/pipeline/spsc_ring.hpp"
#include "support/payload_json.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

using namespace std::chrono_literals;

// Counts what the processor writes; can be told to fail writes or finish.
class CountingOutput final : public SymbolOutput {
public:
    void market_row(const RecordView&, std::string_view) noexcept override {
        rows.fetch_add(1);
        if (fail_after >= 0 && rows.load() >= fail_after) {
            write_failed = true;
        }
    }
    void snapshot_row(const RecordView&, std::size_t, std::string_view) noexcept override {}
    void book_row(std::int64_t, std::uint64_t, std::int32_t, Emit,
                  const TopOfBook&) noexcept override {}
    std::size_t market_rows() const noexcept override {
        return static_cast<std::size_t>(rows.load());
    }
    bool failed() const noexcept override { return write_failed; }
    bool flush() noexcept override {
        flushes.fetch_add(1);
        return !write_failed;
    }
    bool finish() noexcept override {
        finishes.fetch_add(1);
        return !write_failed && !fail_finish;
    }

    std::atomic<int> rows{0};
    std::atomic<int> flushes{0};
    std::atomic<int> finishes{0};
    int fail_after = -1;
    bool fail_finish = false;
    bool write_failed = false;
};

struct Rig {
    explicit Rig(std::size_t ring_bytes = 1u << 16) : ring(ring_bytes) {
        auto out = std::make_unique<CountingOutput>();
        output = out.get();
        std::vector<std::unique_ptr<SymbolOutput>> outputs;
        outputs.push_back(std::move(out));
        processor = std::make_unique<Processor>(
            ProcessorConfig{Venue::Spot, parse_symbol_list(Venue::Spot, "BTCUSDT"), true},
            std::move(outputs), requester);
        config.steady_now_ns = [this] { return clock.load(); };
        config.started_at_ns = 0;
        config.io = nullptr;
        config.on_stats = [this](const std::string& line) {
            const std::lock_guard<std::mutex> lock(mutex);
            stats_lines.push_back(line);
        };
    }

    bool push(std::uint64_t seq) {
        RecordHeader h;
        h.kind = RecordKind::Market;
        h.stream = StreamKind::Trade;
        h.conn_seq = seq;
        h.recv_ns = 1791669450'000'000'000 + static_cast<std::int64_t>(seq);
        return ring.try_push(h, trade);
    }
    std::vector<std::string> lines() {
        const std::lock_guard<std::mutex> lock(mutex);
        return stats_lines;
    }

    const std::string trade = test::trade_json(Venue::Spot, "BTCUSDT", true);
    SpscRing ring;
    NullRequester requester;
    CountingOutput* output = nullptr;
    std::unique_ptr<Processor> processor;
    std::atomic<std::int64_t> clock{0};
    std::atomic<int> fatal_code{0};
    ConsumerConfig config;
    std::mutex mutex;
    std::vector<std::string> stats_lines;
};

template <class Pred>
bool eventually(Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

TEST(Consumer, CloseWithEmptyRingTerminates) {
    Rig rig;
    rig.ring.close_producer();
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(run_consumer(rig.ring, *rig.processor, rig.fatal_code, rig.config), 0);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
    EXPECT_EQ(rig.output->finishes.load(), 1);
    EXPECT_EQ(rig.fatal_code.load(), 0);
}

TEST(Consumer, CloseWithFullRingDrainsEverything) {
    Rig rig(4096);
    std::uint64_t pushed = 0;
    while (rig.push(pushed)) {
        ++pushed;
    }
    ASSERT_GT(pushed, 3u);
    rig.ring.close_producer();
    EXPECT_EQ(run_consumer(rig.ring, *rig.processor, rig.fatal_code, rig.config), 0);
    EXPECT_EQ(static_cast<std::uint64_t>(rig.output->rows.load()), pushed);
    RecordView v;
    EXPECT_FALSE(rig.ring.try_peek(v));
}

TEST(Consumer, WriterFailurePublishesFatalAndDrains) {
    Rig rig;
    rig.output->fail_after = 3;
    for (std::uint64_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(rig.push(i));
    }
    rig.ring.close_producer();
    EXPECT_EQ(run_consumer(rig.ring, *rig.processor, rig.fatal_code, rig.config), 1);
    EXPECT_EQ(rig.fatal_code.load(), 1);
    EXPECT_EQ(rig.output->rows.load(), 3);  // records 4..10 popped but not written
    RecordView v;
    EXPECT_FALSE(rig.ring.try_peek(v));
    EXPECT_EQ(rig.output->finishes.load(), 1);
}

TEST(Consumer, FinishFailureReturnsOne) {
    Rig rig;
    rig.output->fail_finish = true;
    ASSERT_TRUE(rig.push(0));
    rig.ring.close_producer();
    EXPECT_EQ(run_consumer(rig.ring, *rig.processor, rig.fatal_code, rig.config), 1);
    EXPECT_EQ(rig.fatal_code.load(), 1);
}

TEST(Consumer, PeriodicFlushAndStatsFollowInjectedClock) {
    Rig rig;
    rig.config.flush_every = 1000ms;
    rig.config.stats_every = 10s;
    int code = -1;
    std::thread consumer(
        [&] { code = run_consumer(rig.ring, *rig.processor, rig.fatal_code, rig.config); });
    ASSERT_TRUE(rig.push(0));
    ASSERT_TRUE(eventually([&] { return rig.output->rows.load() == 1; }));
    std::this_thread::sleep_for(120ms);  // a few park timeouts with the clock standing still
    EXPECT_EQ(rig.output->flushes.load(), 0);
    EXPECT_TRUE(rig.lines().empty());

    rig.clock.store(1'000'000'000);  // 1 s
    EXPECT_TRUE(eventually([&] { return rig.output->flushes.load() == 1; }));
    EXPECT_TRUE(rig.lines().empty());

    rig.clock.store(10'000'000'000);  // 10 s: the first stats line
    ASSERT_TRUE(eventually([&] { return rig.lines().size() == 1; }));
    EXPECT_EQ(rig.lines()[0].rfind("stats uptime_s=10 ", 0), 0u) << rig.lines()[0];
    std::this_thread::sleep_for(120ms);
    EXPECT_EQ(rig.lines().size(), 1u);

    rig.ring.close_producer();
    consumer.join();
    EXPECT_EQ(code, 0);
    const auto lines = rig.lines();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[1].rfind("summary ", 0), 0u) << lines[1];
}

TEST(Consumer, StatsIncludeIoMetricsAndUptime) {
    Rig rig;
    IoMetrics io;
    io.frames_received.store(5);
    io.reconnects.store(2);
    rig.config.io = &io;
    rig.config.started_at_ns = 3'000'000'000;
    rig.config.stats_every = 0s;  // no periodic lines
    rig.clock.store(15'000'000'000);
    ASSERT_TRUE(rig.push(0));
    rig.ring.close_producer();
    EXPECT_EQ(run_consumer(rig.ring, *rig.processor, rig.fatal_code, rig.config), 0);
    const auto lines = rig.lines();
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].rfind("summary uptime_s=12 frames=5 md_rows=1 ", 0), 0u) << lines[0];
    EXPECT_NE(lines[0].find(" reconnects=2 "), std::string::npos);
}

TEST(Consumer, ConcurrentProducerUnderTsan) {
    Rig rig;
    rig.config.steady_now_ns = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    rig.config.started_at_ns = rig.config.steady_now_ns();
    rig.config.flush_every = 5ms;
    constexpr std::uint64_t kCount = 100'000;
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!rig.push(i)) {
                std::this_thread::yield();
            }
        }
        rig.ring.close_producer();
    });
    EXPECT_EQ(run_consumer(rig.ring, *rig.processor, rig.fatal_code, rig.config), 0);
    producer.join();
    EXPECT_EQ(static_cast<std::uint64_t>(rig.output->rows.load()), kCount);
    EXPECT_EQ(rig.output->finishes.load(), 1);
}

}  // namespace
}  // namespace blc
