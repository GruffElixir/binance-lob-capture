#include "bench.hpp"
#include "blc/core/decimal.hpp"
#include "blc/pipeline/spsc_ring.hpp"
#include "blc/replay/replay.hpp"

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace blc::bench {
namespace {
volatile std::uint64_t checksum = 0;
double percentile(const std::vector<std::int64_t>& values, std::size_t numerator,
                  std::size_t denominator) {
    const std::size_t rank = (values.size() * numerator + denominator - 1) / denominator;
    return static_cast<double>(values.at(rank - 1));
}
struct Scratch {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("blc-replay-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
}  // namespace

void decimal() {
    constexpr std::array<std::string_view, 8> inputs{
        "0",     "0.00000001",          "123456.12345678", "987.00000000",
        "12.34", "9999999999.99999999", "1.0000000000",    "0.00100000"};
    std::vector<double> repetitions;
    for (int repeat = 0; repeat < 10; ++repeat) {
        std::uint64_t sum = 0;
        const auto start = Clock::now();
        for (std::size_t i = 0; i < kIterations; ++i) {
            std::int64_t value = 0;
            if (parse_e8(inputs[i % inputs.size()], value) != DecimalError::Ok)
                throw std::runtime_error("decimal benchmark rejected valid input");
            sum += static_cast<std::uint64_t>(value);
        }
        repetitions.push_back(elapsed_ns(start) / static_cast<double>(kIterations));
        checksum = sum;
    }
    report("decimal", repetitions);
}

void ring() {
    for (const std::size_t bytes : {std::size_t{256}, std::size_t{4096}}) {
        std::vector<double> repetitions;
        for (int repeat = 0; repeat < 10; ++repeat) {
            SpscRing queue(1 << 21);
            const std::string payload(bytes, 'x');
            std::atomic<bool> ready{false};
            std::atomic<bool> go{false};
            std::atomic<bool> valid{true};
            std::thread consumer([&] {
                ready.store(true, std::memory_order_release);
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (std::size_t i = 0; i < kIterations; ++i) {
                    RecordView record;
                    while (!queue.try_peek(record))
                        std::this_thread::yield();
                    if (record.header.conn_seq != i + 1 || record.payload.size != bytes ||
                        record.payload.data[0] != 'x' || record.payload.data[bytes - 1] != 'x')
                        valid.store(false, std::memory_order_relaxed);
                    queue.pop();
                }
            });
            while (!ready.load(std::memory_order_acquire))
                std::this_thread::yield();
            const auto start = Clock::now();
            go.store(true, std::memory_order_release);
            RecordHeader header;
            for (std::size_t i = 0; i < kIterations; ++i) {
                header.conn_seq = i + 1;
                while (!queue.try_push(header, payload))
                    std::this_thread::yield();
            }
            consumer.join();
            repetitions.push_back(elapsed_ns(start) / static_cast<double>(kIterations));
            if (!valid.load())
                throw std::runtime_error("ring ordering or payload failure");
        }
        report("ring payload=" + std::to_string(bytes), repetitions);
        std::sort(repetitions.begin(), repetitions.end());
        const double ns = (repetitions[4] + repetitions[5]) / 2;
        std::cout << "ring records/s=" << 1e9 / ns
                  << " payload_MB/s=" << static_cast<double>(bytes) * 1e3 / ns << '\n';
    }
}

void replay(const std::string& input) {
    std::ifstream file(input);
    if (!file)
        throw std::runtime_error("cannot open replay benchmark input");
    std::string line;
    std::size_t rows = 0;
    while (std::getline(file, line))
        ++rows;
    if (rows < 2)
        throw std::runtime_error("replay benchmark needs market-data rows");
    --rows;
    for (int repeat = 0; repeat < 10; ++repeat) {
        Scratch scratch;
        ReplayOptions options;
        options.inputs = {input};
        options.output_dir = scratch.path;
        options.stats_interval_s = 0;
        std::vector<std::int64_t> latencies;
        latencies.reserve(rows);
        options.record_latency_observer = [&](std::int64_t ns) { latencies.push_back(ns); };
        const auto start = Clock::now();
        if (run_replay(options) != 0)
            throw std::runtime_error("replay benchmark failed");
        const double elapsed = elapsed_ns(start);
        if (latencies.size() != rows)
            throw std::runtime_error("replay observer row mismatch");
        std::sort(latencies.begin(), latencies.end());
        std::cout << "replay repetition=" << repeat + 1 << " rows=" << rows
                  << " records/s=" << static_cast<double>(rows) * 1e9 / elapsed
                  << " p50_ns=" << percentile(latencies, 50, 100)
                  << " p99_ns=" << percentile(latencies, 99, 100)
                  << " p99.9_ns=" << percentile(latencies, 999, 1000) << '\n';
    }
}
}  // namespace blc::bench
