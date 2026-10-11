#include "blc/core/symbols.hpp"
#include "blc/pipeline/output_files.hpp"
#include "blc/pipeline/processor.hpp"
#include "blc/pipeline/snapshot_requester.hpp"
#include "blc/util/logging.hpp"
#include "support/counting_allocator.hpp"
#include "support/exchange_sim.hpp"
#include "support/payload_json.hpp"
#include "support/temp_dir.hpp"

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

using test::ExchangeSim;
using test::OwnedRecord;

constexpr std::int64_t P = kPriceScale;

class RecordingRequester final : public SnapshotRequester {
public:
    void request(std::uint16_t symbol, std::uint32_t epoch) noexcept override {
        wanted.push_back({symbol, epoch});
    }
    struct Want {
        std::uint16_t symbol;
        std::uint32_t epoch;
    };
    std::vector<Want> wanted;
};

std::vector<std::unique_ptr<SymbolOutput>> file_outputs(const std::filesystem::path& dir,
                                                        Venue venue,
                                                        const std::vector<SymbolSpec>& symbols,
                                                        char* scratch) {
    std::vector<std::unique_ptr<SymbolOutput>> outputs;
    for (const auto& s : symbols) {
        outputs.push_back(std::make_unique<FileOutput>(
            std::make_unique<SymbolFiles>(dir, file_names(venue, s.upper, "2026-10-11"), scratch,
                                          SymbolFiles::kBufferBytes),
            venue, s.upper));
    }
    return outputs;
}

// Adds whitespace after the opening brace, so the processor must minify the payload.
std::string prettify(std::string payload) {
    payload.insert(1, "\n  ");
    return payload;
}

// Generates the input once, unmeasured, by running a reference processor and answering its
// snapshot requests from the simulator. The measured processor then sees the same records and,
// being deterministic, takes the same paths.
struct Scenario {
    Venue venue;
    std::vector<SymbolSpec> symbols;
    std::vector<OwnedRecord> records;
};

void generate_symbol0(Scenario& sc, Processor& reference, RecordingRequester& req,
                      std::uint32_t& epoch_base, std::uint32_t seed, ExchangeSim::Options options,
                      int steps, int snapshot_delay) {
    ExchangeSim sim(sc.venue, seed, options);
    const std::string& sym = sc.symbols[0].upper;
    std::uint32_t epoch = epoch_base;  // each simulator run continues the epoch sequence
    std::uint64_t seq = 0;
    std::int64_t t = 1791669450'000'000'000;
    int step = 0;
    struct Pending {
        int deliver_at;
        std::uint32_t epoch;
        std::string body;
    };
    std::deque<Pending> pending;
    const auto add = [&](OwnedRecord r) {
        reference.process(r.view());
        sc.records.push_back(std::move(r));
        for (const auto& w : req.wanted) {
            if (w.symbol == 0) {
                pending.push_back({step + snapshot_delay, w.epoch,
                                   test::snapshot_json(sc.venue, sim.snapshot())});
            }
        }
        req.wanted.clear();
    };
    for (; step < steps; ++step) {
        ExchangeSim::Step s = sim.next();
        std::string payload;
        StreamKind kind = StreamKind::Trade;
        switch (s.kind) {
            case ExchangeSim::Step::Reconnect:
                epoch = epoch_base + s.epoch;
                seq = 0;
                continue;
            case ExchangeSim::Step::Diff:
                kind = StreamKind::DepthDiff;
                payload = test::diff_json(sc.venue, sym, s.diff);
                break;
            case ExchangeSim::Step::Depth5:
                kind = StreamKind::Depth5;
                if (step % 97 == 0 && s.d5.n_bids > 0) {
                    s.d5.bids[0].qty += 1;  // a mismatch: real rate-limited diagnostics
                }
                payload = test::depth5_json(sc.venue, sym, s.d5);
                break;
            case ExchangeSim::Step::Trade:
                payload = test::trade_json(sc.venue, sym, s.trade.buyer_is_maker, seq);
                break;
        }
        if (step % 41 == 0) {
            payload = prettify(payload);
        }
        add(test::market_record(kind, 0, epoch, seq++, t++, payload));
        while (!pending.empty() && pending.front().deliver_at <= step) {
            Pending p = std::move(pending.front());
            pending.pop_front();
            if (p.epoch == epoch) {  // an older connection's response is discarded
                add(test::snapshot_record(0, p.epoch, t++,
                                          step % 3 == 0 ? prettify(p.body) : p.body));
            }
        }
    }
    epoch_base = epoch + 1;
}

// Symbol 1: a hand-built sync, then one diff adding 6 000 bid levels below the book, which
// trims the side to 4 000 levels and revokes completeness.
void generate_symbol1(Scenario& sc, Processor& reference) {
    const std::string& sym = sc.symbols[1].upper;
    const bool usdm = sc.venue == Venue::Usdm;
    std::int64_t t = 1791669460'000'000'000;
    const auto add = [&](OwnedRecord r) {
        reference.process(r.view());
        sc.records.push_back(std::move(r));
    };
    add(test::market_record(
        StreamKind::DepthDiff, 1, 0, 0, t++,
        test::diff_json(sc.venue, sym, test::make_diff(101, 101, 100, {{100'000 * P, P}}, {}))));
    DepthSnapshot s;
    s.last_update_id = usdm ? 101 : 100;
    for (std::int64_t i = 0; i < 10; ++i) {
        s.bids.push_back({(100'000 - i) * P, P});
        s.asks.push_back({(100'001 + i) * P, P});
    }
    add(test::snapshot_record(1, 0, t++, test::snapshot_json(sc.venue, s)));
    std::vector<Level> many;
    for (std::int64_t i = 0; i < 6000; ++i) {
        many.push_back({(50'000 - i) * P, P});
    }
    add(test::market_record(
        StreamKind::DepthDiff, 1, 0, 1, t++,
        test::diff_json(sc.venue, sym, test::make_diff(102, 102, 101, many, {}))));
    add(test::market_record(StreamKind::Trade, 1, 0, 2, t++,
                            test::trade_json(sc.venue, sym, false)));
}

struct Param {
    Venue venue;
    bool trade_rows;
};

class Allocations : public ::testing::TestWithParam<Param> {};

TEST_P(Allocations, ProcessorPathAllocatesNothing) {
    const Param param = GetParam();
    test::TempDir ref_dir;
    test::TempDir dir;
    Scenario sc{param.venue, parse_symbol_list(param.venue, "BTCUSDT,ETHUSDT"), {}};
    sc.records.reserve(40'000);
    auto ref_scratch = std::make_unique<char[]>(SymbolFiles::kBufferBytes);
    RecordingRequester ref_req;
    Processor reference(ProcessorConfig{param.venue, sc.symbols, param.trade_rows},
                        file_outputs(ref_dir.path(), param.venue, sc.symbols, ref_scratch.get()),
                        ref_req);
    ExchangeSim::Options normal;
    normal.p_gap = 1.0 / 1500;
    normal.p_reconnect = 1.0 / 3000;
    std::uint32_t epoch_base = 0;
    generate_symbol0(sc, reference, ref_req, epoch_base, 11, normal, 9'000, 2);
    ExchangeSim::Options drift;
    drift.drift_down = true;
    drift.initial_levels = 1200;
    generate_symbol0(sc, reference, ref_req, epoch_base, 12, drift, 6'000, 3);
    ExchangeSim::Options slow;  // a snapshot delayed past the window's capacity
    generate_symbol0(sc, reference, ref_req, epoch_base, 13, slow, 8'000, 3'000);
    generate_symbol1(sc, reference);

    // The scenario covers what it claims to.
    const ProcessorStats ref = reference.stats();
    EXPECT_GT(ref.totals.syncs, 3u);
    EXPECT_GT(ref.totals.resyncs, 0u);
    EXPECT_GT(ref.totals.gaps + ref.totals.resets, 2u);
    EXPECT_GT(ref.totals.coverage_exhausted, 0u);
    EXPECT_GT(ref.totals.window_trimmed, 0u);
    EXPECT_GT(ref.totals.depth5_full_checks, 0u);
    EXPECT_GT(ref.totals.depth5_mismatch, 0u);
    EXPECT_GT(ref.payloads_minified, 0u);
    if (param.venue == Venue::Usdm) {
        EXPECT_GT(ref.totals.depth5_partial_checks, 0u);
    }
    ASSERT_TRUE(reference.finish());

    // Measured run: counting starts right after construction, with no warm-up.
    auto scratch = std::make_unique<char[]>(SymbolFiles::kBufferBytes);
    NullRequester none;
    Processor measured(ProcessorConfig{param.venue, sc.symbols, param.trade_rows},
                       file_outputs(dir.path(), param.venue, sc.symbols, scratch.get()), none);
    set_log_allocation_hook(&test::suspend_allocation_counting);
    test::arm_allocation_counting();
    for (const auto& r : sc.records) {
        measured.process(r.view());
    }
    measured.flush();
    const std::uint64_t allocations = test::disarm_allocation_counting();
    set_log_allocation_hook(nullptr);
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(measured.stats().ob_rows, ref.ob_rows);
    EXPECT_TRUE(measured.finish());
}

INSTANTIATE_TEST_SUITE_P(All, Allocations,
                         ::testing::Values(Param{Venue::Spot, true}, Param{Venue::Spot, false},
                                           Param{Venue::Usdm, true}, Param{Venue::Usdm, false}),
                         [](const ::testing::TestParamInfo<Param>& param_info) {
                             return std::string(to_string(param_info.param.venue)) +
                                    (param_info.param.trade_rows ? "_trades" : "_no_trades");
                         });

}  // namespace
}  // namespace blc
