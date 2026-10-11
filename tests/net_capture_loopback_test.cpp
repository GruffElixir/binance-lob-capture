#include "blc/core/symbols.hpp"
#include "blc/csv/csv_reader.hpp"
#include "blc/net/capture_app.hpp"
#include "blc/verify/verify.hpp"
#include "support/loopback_server.hpp"

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>

#include <gtest/gtest.h>

namespace blc::test {
namespace {
using namespace std::chrono_literals;
int scale() {
    const auto* value = std::getenv("BLC_TEST_TIME_SCALE");
    return value ? std::max(1, std::atoi(value)) : 1;
}
std::chrono::seconds timeout() {
    return std::chrono::seconds(5 * scale());
}
const TestCa& ca() {
    static const auto value = make_test_ca();
    return value;
}
std::vector<std::string> frames() {
    return {
        R"({"stream":"btcusdt@depth@100ms","data":{"e":"depthUpdate","s":"BTCUSDT","U":101,"u":101,"b":[],"a":[]}})",
        R"({"stream":"btcusdt@depth5@100ms","data":{"lastUpdateId":101,"bids":[["100","2"]],"asks":[["101","3"]]}})",
        R"({"stream":"btcusdt@trade","data":{"s":"BTCUSDT","m":true}})"};
}
CaptureConfig config(LoopbackServer& server, const std::string& name) {
    CaptureConfig c;
    c.options.symbols = parse_symbol_list(Venue::Spot, "BTCUSDT");
    c.options.duration_s = scale();
    c.options.stats_interval_s = 0;
    c.options.output_dir = std::filesystem::temp_directory_path() / ("blc_loopback_" + name);
    std::filesystem::remove_all(c.options.output_dir);
    c.endpoints = server.endpoints(Venue::Spot);
    c.tls.extra_ca_pem = ca().ca_pem;
    c.tuning.backoff_initial = 10ms * scale();
    c.tuning.backoff_cap = 100ms * scale();
    c.tuning.backoff_jitter = 0;
    c.tuning.shutdown_deadline = 1s * scale();
    c.tuning.close_timeout = 500ms * scale();
    c.tuning.poll_interval *= scale();
    c.tuning.ring_retry *= scale();
    return c;
}
TEST(CaptureLoopback, ConnectSyncAndRows) {
    ServerScript script;
    script.frames = frames();
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "sync");
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 0);
    std::vector<std::filesystem::path> all;
    std::filesystem::path book;
    for (const auto& entry : std::filesystem::directory_iterator(c.options.output_dir)) {
        all.push_back(entry.path());
        if (entry.path().filename().string().find("_orderbook.csv") != std::string::npos)
            book = entry.path();
    }
    ASSERT_EQ(all.size(), 3u);
    ASSERT_TRUE(verify_files({all, std::nullopt}).ok);
    {
        auto reader = CsvReader::open(book);
        std::vector<std::string_view> row;
        ASSERT_TRUE(reader.next(row));
        ASSERT_TRUE(reader.next(row));
        ASSERT_EQ(row.size(), 26u);
        EXPECT_EQ(row[4], "R");
        EXPECT_EQ(row[6], "10000000000");
        EXPECT_EQ(row[11], "200000000");
        EXPECT_EQ(row[16], "10100000000");
        EXPECT_EQ(row[21], "300000000");
    }
    std::filesystem::remove_all(c.options.output_dir);
}
TEST(CaptureLoopback, ShutdownDuringTlsHandshake) {
    ServerScript script;
    script.frames = frames();
    script.tls_delay = 10s * scale();
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "shutdown_tls");
    c.options.duration_s = 0;
    auto result = std::async(std::launch::async,
                             [&] { return run_capture(c, std::chrono::steady_clock::now()); });
    EXPECT_TRUE(server.wait_for("tls", 1, timeout()));
    std::raise(SIGINT);
    const bool ready = result.wait_for(timeout()) == std::future_status::ready;
    EXPECT_TRUE(ready) << "shutdown during TLS handshake failed to terminate";
    if (!ready)
        std::raise(SIGTERM);  // ensure a failing assertion cannot strand the test process
    EXPECT_EQ(result.get(), 0);
    EXPECT_EQ(server.count("upgrade"), 0u);
    std::filesystem::remove_all(c.options.output_dir);
}

using Rows = std::vector<std::vector<std::string>>;
Rows rows(const CaptureConfig& c, std::string_view suffix) {
    Rows result;
    if (!std::filesystem::exists(c.options.output_dir))
        return result;
    for (const auto& entry : std::filesystem::directory_iterator(c.options.output_dir)) {
        if (entry.path().filename().string().find(suffix) == std::string::npos)
            continue;
        auto reader = CsvReader::open(entry.path());
        std::vector<std::string_view> row;
        if (!reader.next(row))
            continue;
        while (reader.next(row))
            result.emplace_back(row.begin(), row.end());
    }
    return result;
}
int stop_after(LoopbackServer& server, const CaptureConfig& c, std::string event,
               std::size_t count = 1, int signal = SIGINT) {
    auto result = std::async(std::launch::async,
                             [&] { return run_capture(c, std::chrono::steady_clock::now()); });
    EXPECT_TRUE(server.wait_for(std::move(event), count, timeout()));
    std::raise(signal);
    const bool ready = result.wait_for(timeout()) == std::future_status::ready;
    EXPECT_TRUE(ready);
    if (!ready)
        std::raise(SIGTERM);
    return result.get();
}
void expect_epoch_sync(const CaptureConfig& c, std::uint32_t epoch) {
    const auto book = rows(c, "_orderbook.csv");
    std::map<std::pair<std::string, std::string>, std::string> epochs;
    for (const auto& row : rows(c, "market_data_"))
        epochs[{row[0], row[1]}] = row[5];
    for (const auto& row : rows(c, "depth_snapshots_"))
        epochs[{row[0], row[1]}] = row[4];
    bool found = false;
    for (const auto& row : book) {
        if (epochs[{row[0], row[1]}] == std::to_string(epoch)) {
            EXPECT_EQ(row[4], "R");
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}
TEST(CaptureLoopback, ForcedReconnectStartsNewEpoch) {
    ServerScript script;
    script.frames = frames();
    script.force_close = true;
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "reconnect");
    c.options.duration_s = 2 * scale();
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 0);
    expect_epoch_sync(c, 0);
    expect_epoch_sync(c, 1);
}
TEST(CaptureLoopback, Rest429Cooldown) {
    ServerScript script;
    script.frames = frames();
    script.depth_replies = {
        {429, R"({"code":-1003})", scale()},
        {200, R"({"lastUpdateId":100,"bids":[["100","2"]],"asks":[["101","3"]]})", 0}};
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "cooldown");
    c.options.duration_s = 3 * scale();
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 0);
    const auto times = server.depth_times();
    ASSERT_GE(times.size(), 2u);
    EXPECT_GE(times[1] - times[0], 1s * scale());
    expect_epoch_sync(c, 0);
}
void loss_case(std::string bad, const std::string& name) {
    ServerScript script;
    script.frames = frames();
    script.first_connection_frames = {std::move(bad)};
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, name);
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 0);
    EXPECT_GE(server.count("open"), 2u);
    for (const auto& row : rows(c, "market_data_"))
        EXPECT_NE(row[5], "0");
    expect_epoch_sync(c, 1);
}
TEST(CaptureLoopback, LossTransition) {
    loss_case("not JSON", "loss");
}
TEST(CaptureLoopback, InvalidJsonDataIsLoss) {
    loss_case(R"({"stream":"btcusdt@trade","data":{"s":"BTCUSDT","m":tru}})", "inner_json");
}
TEST(CaptureLoopback, OversizedFrameRejected) {
    loss_case(std::string(1536 * 1024, 'x'), "oversized_frame");
}
TEST(CaptureLoopback, SigintClean) {
    ServerScript script;
    script.frames = frames();
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "sigint");
    c.options.duration_s = 0;
    EXPECT_EQ(stop_after(server, c, "frame", 3), 0);
    expect_epoch_sync(c, 0);
}
TEST(CaptureLoopback, SigtermClean) {
    ServerScript script;
    script.frames = frames();
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "sigterm");
    c.options.duration_s = 0;
    EXPECT_EQ(stop_after(server, c, "frame", 3, SIGTERM), 0);
    expect_epoch_sync(c, 0);
}
TEST(CaptureLoopback, SecondSigint) {
    ServerScript script;
    script.frames = frames();
    script.hold_close = true;
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "second_signal");
    c.options.duration_s = 0;
    c.tuning.close_timeout = c.tuning.shutdown_deadline = 20s * scale();
    auto result = std::async(std::launch::async,
                             [&] { return run_capture(c, std::chrono::steady_clock::now()); });
    EXPECT_TRUE(server.wait_for("frame", 3, timeout()));
    std::raise(SIGINT);
    EXPECT_TRUE(server.wait_for("close_requested", 1, timeout()));
    std::raise(SIGINT);
    const bool ready = result.wait_for(timeout()) == std::future_status::ready;
    EXPECT_TRUE(ready);
    if (!ready)
        std::raise(SIGTERM);
    EXPECT_EQ(result.get(), 0);
    expect_epoch_sync(c, 0);
}
TEST(CaptureLoopback, DurationExpiry) {
    ServerScript script;
    script.frames = frames();
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "duration");
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(run_capture(c, start), 0);
    EXPECT_LE(std::chrono::steady_clock::now() - start, timeout());
    expect_epoch_sync(c, 0);
}
TEST(CaptureLoopback, ShutdownDuringWsHandshake) {
    ServerScript script;
    script.frames = frames();
    script.ws_delay = 10s * scale();
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "shutdown_ws");
    c.options.duration_s = 0;
    EXPECT_EQ(stop_after(server, c, "upgrade"), 0);
    EXPECT_EQ(server.count("open"), 0u);
    EXPECT_EQ(server.count("closed"), 0u);
}
TEST(CaptureLoopback, DurationExpiryDuringHandshakes) {
    for (bool tls : {true, false}) {
        ServerScript script;
        script.frames = frames();
        (tls ? script.tls_delay : script.ws_delay) = 10s * scale();
        LoopbackServer server(std::move(script), ca());
        auto c = config(server, tls ? "duration_tls" : "duration_ws");
        const auto start = std::chrono::steady_clock::now();
        EXPECT_EQ(run_capture(c, start), 0);
        EXPECT_LE(std::chrono::steady_clock::now() - start, timeout());
        EXPECT_EQ(server.count("open"), 0u);
        EXPECT_EQ(server.count("closed"), 0u);
    }
}
TEST(CaptureLoopback, ShutdownWhileWaiting) {
    ServerScript script;
    script.refuse_ws = true;
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "waiting");
    c.options.duration_s = 0;
    c.tuning.backoff_initial = c.tuning.backoff_cap = 10s * scale();
    EXPECT_EQ(stop_after(server, c, "attempt"), 0);
    EXPECT_LE(server.count("attempt"), 1u);
}
TEST(CaptureLoopback, AttemptLimiterUnderStorm) {
    ServerScript script;
    script.force_close = true;
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "storm");
    c.tuning.limiter_attempts = 5;
    c.tuning.limiter_window = 2s * scale();
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 0);
    EXPECT_LE(server.count("attempt"), 5u);
    EXPECT_GT(server.count("attempt"), 1u);
}
TEST(CaptureLoopback, InvalidSymbolExit2) {
    LoopbackServer server({}, ca());
    auto c = config(server, "invalid_symbol");
    c.options.symbols = parse_symbol_list(Venue::Spot, "BTCUSDX");
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 2);
    EXPECT_FALSE(std::filesystem::exists(c.options.output_dir));
    EXPECT_EQ(server.count("attempt"), 0u);
}
TEST(CaptureLoopback, ReversedEnvelopeFrames) {
    ServerScript script;
    script.frames = {
        R"({"data":{"e":"depthUpdate","s":"BTCUSDT","U":101,"u":101,"b":[],"a":[]},"stream":"btcusdt@depth@100ms"})",
        R"({"data":{"lastUpdateId":101,"bids":[["100","2"]],"asks":[["101","3"]]},"stream":"btcusdt@depth5@100ms"})"};
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "reversed");
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 0);
    EXPECT_EQ(server.count("open"), 1u);
    expect_epoch_sync(c, 0);
    ASSERT_FALSE(rows(c, "_orderbook.csv").empty());
    EXPECT_EQ(rows(c, "_orderbook.csv")[0][6], "10000000000");
}
TEST(CaptureLoopback, UntrustedCertificateRejected) {
    LoopbackServer server({}, ca());
    auto c = config(server, "untrusted");
    c.tls.extra_ca_pem = make_test_ca().ca_pem;
    c.tuning.validation_give_up = 100ms * scale();
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 1);
    EXPECT_FALSE(std::filesystem::exists(c.options.output_dir));
    EXPECT_EQ(server.count("ticker"), 0u);
}
TEST(CaptureLoopback, Snapshot400ExitsOne) {
    ServerScript script;
    script.frames = frames();
    script.depth_replies = {{400, R"({"code":-1121,"msg":"Invalid symbol."})", 0}};
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "snapshot400");
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 1);
    EXPECT_EQ(server.count("ticker"), 1u);
    EXPECT_EQ(server.count("depth"), 1u);
}
class FailingOutput final : public SymbolOutput {
public:
    void market_row(const RecordView&, std::string_view) noexcept override { ++count_; }
    void snapshot_row(const RecordView&, std::size_t, std::string_view) noexcept override {}
    void book_row(std::int64_t, std::uint64_t, std::int32_t, Emit,
                  const TopOfBook&) noexcept override {}
    std::size_t market_rows() const noexcept override { return count_; }
    bool failed() const noexcept override { return count_ >= 5; }
    bool flush() noexcept override { return !failed(); }
    bool finish() noexcept override { return !failed(); }

private:
    std::size_t count_ = 0;
};
TEST(CaptureLoopback, WriterFailureExitsOne) {
    ServerScript script;
    script.frames = frames();
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "writer_failure");
    c.output_factory = [](const SymbolSpec&) { return std::make_unique<FailingOutput>(); };
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(run_capture(c, start), 1);
    EXPECT_LE(std::chrono::steady_clock::now() - start, timeout());
}
TEST(CaptureLoopback, DurationDuringValidation) {
    ServerScript script;
    script.ticker_delay = 10s * scale();
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "validation_duration");
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 0);
    EXPECT_FALSE(std::filesystem::exists(c.options.output_dir));
    EXPECT_EQ(server.count("attempt"), 0u);
}
TEST(CaptureLoopback, AlreadyExpiredDuration) {
    LoopbackServer server({}, ca());
    auto c = config(server, "expired");
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now() - 20s * scale()), 0);
    EXPECT_EQ(server.count("ticker"), 0u);
    EXPECT_EQ(server.count("attempt"), 0u);
}
TEST(CaptureLoopback, FileDateUsesStartBeforeValidation) {
    ServerScript script;
    script.frames = frames();
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "start_date");
    c.started_wall_ns =
        1736899200LL * 1000000000;  // 2025-01-15 UTC, before current validation time
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 0);
    EXPECT_TRUE(
        std::filesystem::exists(c.options.output_dir / "market_data_spot_BTCUSDT_2025-01-15.csv"));
}
void snapshot_retry_case(std::string bad, const std::string& name) {
    ServerScript script;
    script.frames = frames();
    script.depth_replies = {
        {200, std::move(bad), 0},
        {200, R"({"lastUpdateId":100,"bids":[["100","2"]],"asks":[["101","3"]]})", 0}};
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, name);
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 0);
    EXPECT_GE(server.count("depth"), 2u);
    EXPECT_EQ(rows(c, "depth_snapshots_").size(), 1u);
    expect_epoch_sync(c, 0);
}
TEST(CaptureLoopback, OversizedRestBodyRetried) {
    snapshot_retry_case(std::string(1536 * 1024, 'x'), "oversized_rest");
}
TEST(CaptureLoopback, InvalidJsonSnapshotRetried) {
    snapshot_retry_case(R"({"lastUpdateId":100,"bids":[oops],"asks":[]})", "invalid_snapshot");
}
TEST(CaptureLoopback, FragmentedBufferCapacityBounded) {
    ServerScript script;
    script.fragment_bytes = 8192;
    script.frames = {R"({"stream":"btcusdt@trade","data":{"s":"BTCUSDT","m":true,"pad":")" +
                         std::string(200 * 1024, 'x') + "\"}}",
                     R"({"stream":"btcusdt@trade","data":{"s":"BTCUSDT","m":true,"pad":")" +
                         std::string(1000 * 1024, 'x') + "\"}}"};
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "fragmented");
    std::size_t transient = 0, retained = 0;
    c.buffer_capacity_observer = [&](std::uint16_t, std::size_t capacity, bool after) {
        (after ? retained : transient) = std::max(after ? retained : transient, capacity);
    };
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 0);
    EXPECT_LE(transient, 1024u * 1024);
    EXPECT_GT(transient, 256u * 1024);
    EXPECT_LE(retained, 256u * 1024);
    EXPECT_GE(rows(c, "market_data_").size(), 2u);
}
TEST(CaptureLoopback, DurationBeforeSyncExitsZero) {
    ServerScript script;
    script.frames = frames();
    script.depth_delay = 10s * scale();
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "before_sync");
    EXPECT_EQ(run_capture(c, std::chrono::steady_clock::now()), 0);
    EXPECT_GT(server.count("depth"), 0u);
    EXPECT_TRUE(rows(c, "_orderbook.csv").empty());
}
struct RecordedBooks {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::pair<std::uint32_t, char>> rows;
    std::size_t syncs = 0;
};
class RecordingOutput final : public SymbolOutput {
public:
    explicit RecordingOutput(RecordedBooks& books) : books_(books) {}
    void market_row(const RecordView& r, std::string_view) noexcept override {
        epoch_ = r.header.epoch;
        ++count_;
    }
    void snapshot_row(const RecordView& r, std::size_t, std::string_view) noexcept override {
        epoch_ = r.header.epoch;
    }
    void book_row(std::int64_t, std::uint64_t, std::int32_t, Emit emit,
                  const TopOfBook&) noexcept override {
        std::lock_guard<std::mutex> lock(books_.mutex);
        books_.rows.emplace_back(epoch_, emit.type);
        if (emit.type == 'R')
            ++books_.syncs;
        books_.cv.notify_all();
    }
    std::size_t market_rows() const noexcept override { return count_; }
    bool failed() const noexcept override { return false; }
    bool flush() noexcept override { return true; }
    bool finish() noexcept override { return true; }

private:
    RecordedBooks& books_;
    std::uint32_t epoch_ = 0;
    std::size_t count_ = 0;
};
TEST(CaptureLoopback, FaultReconnectEvery) {
    ServerScript script;
    script.frames = frames();
    LoopbackServer server(std::move(script), ca());
    auto c = config(server, "fault_timer");
    c.options.duration_s = 0;
    c.options.fault_reconnect_every_s = scale();
    RecordedBooks books;
    c.output_factory = [&](const SymbolSpec&) { return std::make_unique<RecordingOutput>(books); };
    auto result = std::async(std::launch::async,
                             [&] { return run_capture(c, std::chrono::steady_clock::now()); });
    {
        std::unique_lock<std::mutex> lock(books.mutex);
        EXPECT_TRUE(books.cv.wait_for(lock, 10s * scale(), [&] { return books.syncs >= 3; }));
    }
    std::raise(SIGINT);
    EXPECT_EQ(result.get(), 0);
    std::optional<std::uint32_t> previous;
    for (const auto& row : books.rows) {
        if (!previous || row.first != *previous) {
            if (previous) {
                EXPECT_GT(row.first, *previous);
            }
            EXPECT_EQ(row.second, 'R');
            previous = row.first;
        }
    }
    ASSERT_TRUE(previous);
    EXPECT_GE(*previous, 2u);
}
}  // namespace
}  // namespace blc::test
