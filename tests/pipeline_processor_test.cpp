#include "blc/core/symbols.hpp"
#include "blc/core/time.hpp"
#include "blc/pipeline/output_files.hpp"
#include "blc/pipeline/processor.hpp"
#include "blc/pipeline/snapshot_requester.hpp"
#include "support/payload_json.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

using test::diff_json;
using test::make_depth5;
using test::make_diff;
using test::market_record;
using test::read_file;
using test::snapshot_json;
using test::snapshot_record;
using test::TempDir;
using test::trade_json;

constexpr std::int64_t P = kPriceScale;
constexpr std::int64_t kT0 = 1791669450431220517;

std::vector<std::string> lines(const std::filesystem::path& p) {
    std::vector<std::string> out;
    std::istringstream in(read_file(p));
    std::string line;
    while (std::getline(in, line)) {
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : line) {
        if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

// Snapshot at id 100: bids 100..96, asks 101..105, qty 1 each.
DepthSnapshot base_snapshot() {
    DepthSnapshot s;
    s.last_update_id = 100;
    for (std::int64_t i = 0; i < 5; ++i) {
        s.bids.push_back({(100 - i) * P, 1 * P});
        s.asks.push_back({(101 + i) * P, 1 * P});
    }
    return s;
}

// Real per-symbol files in a temp dir, plus helpers that build records for symbol `i`.
struct Harness {
    explicit Harness(std::vector<std::string> names = {"BTCUSDT"}, Venue v = Venue::Spot,
                     bool trade_rows = true)
        : venue(v), slots(names.size()) {
        std::string list;
        for (const auto& n : names) {
            list += (list.empty() ? "" : ",") + n;
        }
        symbols = parse_symbol_list(venue, list);
        std::vector<std::unique_ptr<SymbolOutput>> outputs;
        for (const auto& s : symbols) {
            files.push_back(file_names(venue, s.upper, "2026-10-11"));
            outputs.push_back(std::make_unique<FileOutput>(
                std::make_unique<SymbolFiles>(dir.path(), files.back(), scratch.get(),
                                              SymbolFiles::kBufferBytes),
                venue, s.upper));
        }
        processor = std::make_unique<Processor>(ProcessorConfig{venue, symbols, trade_rows},
                                                std::move(outputs), slots);
    }

    void diff(std::uint16_t i, std::uint64_t U, std::uint64_t u, std::vector<Level> bids,
              std::vector<Level> asks, std::uint32_t epoch = 0) {
        feed(market_record(StreamKind::DepthDiff, i, epoch, seq++, kT0 + t++,
                           diff_json(venue, symbols[i].upper,
                                     make_diff(U, u, U - 1, std::move(bids), std::move(asks)))));
    }
    void depth5(std::uint16_t i, std::uint64_t id, const std::vector<Level>& bids,
                const std::vector<Level>& asks, std::uint32_t epoch = 0) {
        feed(
            market_record(StreamKind::Depth5, i, epoch, seq++, kT0 + t++,
                          test::depth5_json(venue, symbols[i].upper, make_depth5(id, bids, asks))));
    }
    void trade(std::uint16_t i, bool m = true, std::uint32_t epoch = 0) {
        feed(market_record(StreamKind::Trade, i, epoch, seq++, kT0 + t++,
                           trade_json(venue, symbols[i].upper, m)));
    }
    void raw(StreamKind k, std::uint16_t i, const std::string& payload, std::uint32_t epoch = 0) {
        feed(market_record(k, i, epoch, seq++, kT0 + t++, payload));
    }
    void snapshot(std::uint16_t i, const DepthSnapshot& s, std::uint32_t epoch = 0) {
        feed(snapshot_record(i, epoch, kT0 + t++, snapshot_json(venue, s)));
    }
    void raw_snapshot(std::uint16_t i, const std::string& payload, std::uint32_t epoch = 0) {
        feed(snapshot_record(i, epoch, kT0 + t++, payload));
    }
    void feed(const test::OwnedRecord& r) { processor->process(r.view()); }

    // diff 101 (bid 100 -> qty 2), then the base snapshot: Synced with an R row.
    void sync(std::uint16_t i, std::uint32_t epoch = 0) {
        diff(i, 101, 101, {{100 * P, 2 * P}}, {}, epoch);
        snapshot(i, base_snapshot(), epoch);
    }

    std::vector<std::string> book_lines(std::size_t i) {
        processor->flush();
        return lines(dir / files[i].orderbook);
    }
    std::vector<std::string> market_lines(std::size_t i) {
        processor->flush();
        return lines(dir / files[i].market_data);
    }
    std::vector<std::string> sidecar_lines(std::size_t i) {
        processor->flush();
        return lines(dir / files[i].snapshots);
    }

    TempDir dir;
    Venue venue;
    std::unique_ptr<char[]> scratch = std::make_unique<char[]>(SymbolFiles::kBufferBytes);
    std::vector<SymbolSpec> symbols;
    std::vector<FileNames> files;
    RequestSlots slots;
    std::unique_ptr<Processor> processor;
    std::uint64_t seq = 0;
    std::int64_t t = 0;
};

TEST(Processor, MarketRowBeforeParseEvenOnParseError) {
    Harness h;
    h.raw(StreamKind::Trade, 0, R"({"e":"trade","s":"BTCUSDT"})");  // no "m"
    const auto md = h.market_lines(0);
    ASSERT_EQ(md.size(), 2u);
    EXPECT_EQ(
        md[1],
        R"(1791669450,431220517,spot,trade,0,0,0,BTCUSDT,"{""e"":""trade"",""s"":""BTCUSDT""}")");
    EXPECT_EQ(h.book_lines(0).size(), 1u);
    EXPECT_EQ(h.processor->counters(0).parse_errors, 1u);
}

TEST(Processor, BookRowFormat) {
    Harness h;
    h.sync(0);
    h.depth5(0, 101, {{100 * P, 2 * P}, {99 * P, P}, {98 * P, P}, {97 * P, P}, {96 * P, P}},
             {{101 * P, P}, {102 * P, P}, {103 * P, P}, {104 * P, P}, {105 * P, P}});
    const auto ob = h.book_lines(0);
    ASSERT_EQ(ob.size(), 3u);
    EXPECT_EQ(fields(ob[1])[2], "0");
    EXPECT_EQ(fields(ob[1])[4], "R");
    EXPECT_EQ(h.symbols[0].id, 2014473836);
    EXPECT_EQ(ob[2],
              "1791669450,431220519,1,2014473836,P,N,"
              "10000000000,9900000000,9800000000,9700000000,9600000000,"
              "200000000,100000000,100000000,100000000,100000000,"
              "10100000000,10200000000,10300000000,10400000000,10500000000,"
              "100000000,100000000,100000000,100000000,100000000");
    EXPECT_EQ(std::count(ob[2].begin(), ob[2].end(), ','), 25);
    EXPECT_EQ(h.processor->counters(0).depth5_verified, 1u);
}

TEST(Processor, SeqNoPerSymbol) {
    Harness h({"BTCUSDT", "ETHUSDT"});
    h.sync(0);
    h.sync(1);
    h.trade(1);
    h.trade(0, false);
    h.trade(1, true);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto ob = h.book_lines(i);
        const std::size_t rows = i == 0 ? 2 : 3;
        ASSERT_EQ(ob.size(), rows + 1) << i;
        for (std::size_t r = 0; r < rows; ++r) {
            EXPECT_EQ(fields(ob[r + 1])[2], std::to_string(r));
            EXPECT_EQ(fields(ob[r + 1])[3], std::to_string(h.symbols[i].id));
        }
    }
    EXPECT_EQ(fields(h.book_lines(0)[2])[5], "S");  // m=false -> seller was the maker
    EXPECT_EQ(fields(h.book_lines(1)[3])[5], "B");
}

TEST(Processor, SidecarApplyBeforeRow) {
    Harness h;
    h.diff(0, 101, 101, {{100 * P, 2 * P}}, {});
    h.trade(0);
    h.trade(0);
    h.snapshot(0, base_snapshot());
    const auto sc = h.sidecar_lines(0);
    ASSERT_EQ(sc.size(), 2u);
    const auto f = fields(sc[1]);
    EXPECT_EQ(f[2], "spot");
    EXPECT_EQ(f[3], "0");
    EXPECT_EQ(f[4], "0");
    EXPECT_EQ(f[5], "BTCUSDT");
    EXPECT_EQ(f[6], "3");
    EXPECT_EQ(h.processor->stats().snapshot_rows, 1u);
    EXPECT_EQ(h.book_lines(0).size(), 2u);  // synced: one R row
}

TEST(Processor, RequestsThroughSlots) {
    Harness h;
    EXPECT_EQ(h.slots.value(0), 0u);
    h.diff(0, 101, 101, {{100 * P, 2 * P}}, {});
    EXPECT_EQ(h.slots.value(0), (std::uint64_t{0} << 32) | 1u);
    h.diff(0, 102, 102, {}, {{101 * P, 3 * P}});  // still waiting: no second request
    EXPECT_EQ(h.slots.value(0), 1u);
    h.snapshot(0, base_snapshot());
    ASSERT_EQ(h.book_lines(0).size(), 2u);
    h.diff(0, 200, 201, {}, {{101 * P, 3 * P}});  // gap
    EXPECT_EQ(h.slots.value(0), 2u);
    std::vector<std::pair<std::size_t, std::uint64_t>> seen;
    h.slots.scan([&](std::size_t symbol, std::uint32_t epoch, std::uint32_t number) {
        seen.emplace_back(symbol, (std::uint64_t{epoch} << 32) | number);
    });
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].second, 2u);
    seen.clear();
    h.slots.scan(
        [&](std::size_t symbol, std::uint32_t, std::uint32_t) { seen.emplace_back(symbol, 0); });
    EXPECT_TRUE(seen.empty());  // unchanged since the last scan
}

TEST(Processor, PerSymbolEpochIsolation) {
    Harness h({"BTCUSDT", "ETHUSDT"});
    h.sync(0);
    h.sync(1);
    h.trade(0, true, 1);  // BTC moves to epoch 1: reset, no row
    EXPECT_EQ(h.processor->counters(0).resets, 2u);
    EXPECT_EQ(h.processor->counters(1).resets, 1u);
    h.trade(1);  // ETH still on epoch 0 and Synced
    EXPECT_EQ(h.book_lines(1).size(), 3u);
    EXPECT_EQ(h.book_lines(0).size(), 2u);
    h.trade(1, true, 1);
    EXPECT_EQ(h.processor->counters(1).resets, 2u);
    EXPECT_EQ(h.slots.value(0), (std::uint64_t{1} << 32) | 2u);
}

class FailingOutput final : public SymbolOutput {
public:
    explicit FailingOutput(int fail_after) : fail_after_(fail_after) {}
    void market_row(const RecordView&, std::string_view) noexcept override { ++rows_; }
    void snapshot_row(const RecordView&, std::size_t, std::string_view) noexcept override {
        ++rows_;
    }
    void book_row(std::int64_t, std::uint64_t, std::int32_t, Emit,
                  const TopOfBook&) noexcept override {
        ++rows_;
    }
    std::size_t market_rows() const noexcept override { return 0; }
    bool failed() const noexcept override { return rows_ >= fail_after_; }
    bool flush() noexcept override { return !failed(); }
    bool finish() noexcept override { return !failed(); }
    int rows_ = 0;

private:
    int fail_after_;
};

TEST(Processor, WriterFailureSetsFatal) {
    const auto symbols = parse_symbol_list(Venue::Spot, "BTCUSDT");
    std::vector<std::unique_ptr<SymbolOutput>> outputs;
    outputs.push_back(std::make_unique<FailingOutput>(2));
    auto* out = static_cast<FailingOutput*>(outputs[0].get());
    NullRequester none;
    Processor p(ProcessorConfig{Venue::Spot, symbols, true}, std::move(outputs), none);
    const auto rec =
        market_record(StreamKind::Trade, 0, 0, 0, kT0, trade_json(Venue::Spot, "BTCUSDT", true));
    p.process(rec.view());
    EXPECT_FALSE(p.fatal());
    p.process(rec.view());
    EXPECT_TRUE(p.fatal());
    p.process(rec.view());
    p.process(rec.view());
    EXPECT_EQ(out->rows_, 2);
    EXPECT_FALSE(p.finish());
}

TEST(Processor, PrettyPayloadMinified) {
    Harness h;
    h.raw(StreamKind::Trade, 0, "{ \"e\": \"trade\",\n  \"s\": \"BTCUSDT\",\r\n\t\"m\": true }");
    const auto md = h.market_lines(0);
    ASSERT_EQ(md.size(), 2u);
    EXPECT_EQ(
        md[1],
        R"(1791669450,431220517,spot,trade,0,0,0,BTCUSDT,"{""e"":""trade"",""s"":""BTCUSDT"",""m"":true}")");
    EXPECT_EQ(h.processor->stats().payloads_minified, 1u);
    EXPECT_EQ(h.processor->counters(0).parse_errors, 0u);
    // Whitespace inside strings only: nothing to minify, not counted.
    h.raw(StreamKind::Trade, 0, R"({"e":"trade x","s":"BTCUSDT","m":true})");
    EXPECT_EQ(h.processor->stats().payloads_minified, 1u);
}

TEST(Processor, PrettySnapshotMinified) {
    Harness h;
    h.diff(0, 101, 101, {{100 * P, 2 * P}}, {});
    std::string pretty = snapshot_json(Venue::Spot, base_snapshot());
    std::string spaced;
    for (const char c : pretty) {
        spaced += c;
        if (c == ',' || c == '[') {
            spaced += "\n  ";
        }
    }
    h.raw_snapshot(0, spaced);
    const auto sc = h.sidecar_lines(0);
    ASSERT_EQ(sc.size(), 2u);
    EXPECT_NE(sc[1].find(R"(,1,"{""lastUpdateId"":100,""bids"":[[""100.00000000"")"),
              std::string::npos)
        << sc[1];
    EXPECT_EQ(h.processor->stats().payloads_minified, 1u);
    EXPECT_EQ(h.book_lines(0).size(), 2u);  // synced
}

TEST(Processor, EpochRegressionIgnored) {
    Harness h;
    h.diff(0, 101, 101, {{100 * P, 2 * P}}, {}, 2);
    h.diff(0, 102, 102, {{100 * P, 3 * P}}, {}, 1);
    EXPECT_EQ(h.processor->counters(0).epoch_regressions, 1u);
    EXPECT_EQ(h.market_lines(0).size(), 2u);
    EXPECT_EQ(h.slots.value(0), (std::uint64_t{2} << 32) | 1u);
    EXPECT_EQ(h.processor->stats().md_rows, 1u);
}

TEST(Processor, MalformedDepth5OrTradeDoesNotUnsync) {
    Harness h;
    h.sync(0);
    const std::uint64_t slot = h.slots.value(0);
    h.raw(StreamKind::Depth5, 0, R"({"lastUpdateId":101,"asks":[]})");
    h.raw(StreamKind::Trade, 0, R"({"e":"trade","s":"BTCUSDT","m":"yes"})");
    EXPECT_EQ(h.processor->counters(0).parse_errors, 2u);
    EXPECT_EQ(h.market_lines(0).size(), 4u);  // diff + 2 malformed (snapshot is in the sidecar)
    EXPECT_EQ(h.book_lines(0).size(), 2u);    // only the R row
    EXPECT_EQ(h.slots.value(0), slot);
    h.trade(0);
    EXPECT_EQ(h.book_lines(0).size(), 3u);  // still Synced
}

TEST(Processor, StatsLineKeys) {
    Harness h;
    h.sync(0);
    IoMetrics io;
    io.frames_received.store(394);
    const std::string line = format_stats(h.processor->stats(), &io, 16u << 20, 12 * 1024, 10);
    EXPECT_EQ(line.rfind("stats ", 0), 0u) << line;
    for (const char* key : {"uptime_s=10",
                            "frames=394",
                            "md_rows=1",
                            "ob_rows=1",
                            "parse_errors=0",
                            "gaps=0",
                            "resyncs=0",
                            "reconnects=0",
                            "coverage_exhausted=0",
                            "depth5_verified=0",
                            "depth5_mismatch=0",
                            "ring_drops=0",
                            "ring_hw_kib=12",
                            "retained_kib=",
                            "lat_p50_us=",
                            "lat_p99_us=",
                            "syncs=1",
                            "snapshot_rows=1",
                            "window_trimmed=0",
                            "payloads_minified=0",
                            "depth5_partially_verified=0",
                            "depth5_full_checks=0",
                            "shard_losses=0"}) {
        EXPECT_NE(line.find(std::string(" ") + key), std::string::npos) << key << " in " << line;
    }
    const std::string offline = format_stats(h.processor->stats(), nullptr, 0, 0, 3);
    EXPECT_EQ(offline.find("frames="), std::string::npos);
    EXPECT_EQ(offline.find("reconnects="), std::string::npos);
    EXPECT_NE(offline.find(" md_rows=1"), std::string::npos);
    // retained_kib includes the ring capacity and receive buffers when given.
    const auto kib = [](const std::string& s) {
        const auto at = s.find("retained_kib=") + 13;
        return std::stoull(s.substr(at, s.find(' ', at) - at));
    };
    EXPECT_EQ(kib(line), kib(offline) + 16 * 1024);
}

TEST(LatencyHistogram, Percentiles) {
    LatencyHistogram h;
    EXPECT_EQ(h.percentile(0.5), 0);
    for (std::int64_t us = 1; us <= 1000; ++us) {
        h.add(us * 1000);
    }
    const std::int64_t p50 = h.percentile(0.5);
    EXPECT_GE(p50, 500'000);
    EXPECT_LE(p50, 2 * 500'000);
    EXPECT_GE(h.percentile(0.99), p50);
    EXPECT_GE(h.percentile(1.0), 1'000'000);
    EXPECT_EQ(h.count(), 1000u);
}

TEST(Processor, SnapshotLatencyUsesReceiveTime) {
    Harness h;
    h.diff(0, 101, 101, {{100 * P, 2 * P}}, {});
    RecordHeader hd;
    hd.kind = RecordKind::Snapshot;
    hd.recv_ns = kT0;
    hd.steady_ns = steady_clock_ns() - 5 * kNanosPerSecond;
    const test::OwnedRecord rec(hd, snapshot_json(Venue::Spot, base_snapshot()));
    h.feed(rec);
    const auto stats = h.processor->stats();
    EXPECT_EQ(stats.latency.count(), 1u);  // the diff had no steady time (replay-like)
    EXPECT_GE(stats.latency.percentile(1.0), 5 * kNanosPerSecond);
}

TEST(Processor, RetainedBytesStable) {
    Harness h({"BTCUSDT", "ETHUSDT"});
    const std::size_t before = h.processor->stats().retained_bytes;
    EXPECT_GT(before, std::size_t{1} << 20);  // at least the minify scratch
    h.sync(0);
    h.sync(1);
    std::uint64_t id = 101;
    for (int i = 0; i < 10'000; ++i) {
        const auto sym = static_cast<std::uint16_t>(i % 2);
        if (i % 3 == 0) {
            h.trade(sym, i % 4 == 0);
        } else {
            ++id;
            h.diff(sym, id, id, {{(90 + i % 7) * P, (i % 5) * P}}, {{(106 + i % 9) * P, P}});
        }
    }
    EXPECT_EQ(h.processor->stats().retained_bytes, before);
}

}  // namespace
}  // namespace blc
