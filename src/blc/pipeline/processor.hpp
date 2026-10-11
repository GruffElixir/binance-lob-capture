#pragma once

#include "blc/book/symbol_book.hpp"
#include "blc/core/symbols.hpp"
#include "blc/core/types.hpp"
#include "blc/csv/csv_writer.hpp"
#include "blc/parse/events.hpp"
#include "blc/parse/payload_parser.hpp"
#include "blc/pipeline/metrics.hpp"
#include "blc/pipeline/output_files.hpp"
#include "blc/pipeline/record.hpp"
#include "blc/pipeline/snapshot_requester.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace blc {

// Where one symbol's rows go. Capture writes all three files; replay writes the order book only.
// flush() and finish() check every result; finish() visits every file, is idempotent, and
// failures stay sticky.
class SymbolOutput {
public:
    virtual ~SymbolOutput() = default;
    virtual void market_row(const RecordView& record,
                            std::string_view payload_minified) noexcept = 0;
    virtual void snapshot_row(const RecordView& record, std::size_t apply_before_row,
                              std::string_view payload_minified) noexcept = 0;
    virtual void book_row(std::int64_t recv_ns, std::uint64_t seq, std::int32_t id, Emit emit,
                          const TopOfBook& top) noexcept = 0;
    virtual std::size_t market_rows() const noexcept = 0;
    virtual bool failed() const noexcept = 0;
    virtual bool flush() noexcept = 0;
    virtual bool finish() noexcept = 0;
    // Buffer capacity this output keeps (for the retained_bytes metric).
    virtual std::size_t retained_bytes() const noexcept { return 0; }
};

// Capture: market-data, order-book and sidecar files of one symbol.
class FileOutput final : public SymbolOutput {
public:
    FileOutput(std::unique_ptr<SymbolFiles> files, Venue venue, std::string upper_symbol);

    void market_row(const RecordView& record, std::string_view payload_minified) noexcept override;
    void snapshot_row(const RecordView& record, std::size_t apply_before_row,
                      std::string_view payload_minified) noexcept override;
    void book_row(std::int64_t recv_ns, std::uint64_t seq, std::int32_t id, Emit emit,
                  const TopOfBook& top) noexcept override;
    std::size_t market_rows() const noexcept override { return market_rows_; }
    bool failed() const noexcept override { return files_->failed(); }
    bool flush() noexcept override { return files_->flush(); }
    bool finish() noexcept override { return files_->finish(); }
    std::size_t retained_bytes() const noexcept override { return 2 * SymbolFiles::kBufferBytes; }

private:
    std::unique_ptr<SymbolFiles> files_;
    Venue venue_;
    std::string symbol_;
    std::size_t market_rows_ = 0;
};

// Replay: only the order-book file; market rows are counted (they already exist).
class BookOnlyOutput final : public SymbolOutput {
public:
    explicit BookOnlyOutput(const std::filesystem::path& orderbook_path);

    void market_row(const RecordView&, std::string_view) noexcept override { ++market_rows_; }
    void snapshot_row(const RecordView&, std::size_t, std::string_view) noexcept override {}
    void book_row(std::int64_t recv_ns, std::uint64_t seq, std::int32_t id, Emit emit,
                  const TopOfBook& top) noexcept override;
    std::size_t market_rows() const noexcept override { return market_rows_; }
    bool failed() const noexcept override { return file_.failed(); }
    bool flush() noexcept override;
    bool finish() noexcept override;
    std::size_t retained_bytes() const noexcept override { return SymbolFiles::kBufferBytes; }

private:
    std::unique_ptr<char[]> buffer_;  // NOLINT(modernize-avoid-c-arrays): dynamically sized
                                      // preallocated backing storage
    OutputFile file_;
    std::size_t market_rows_ = 0;
    bool finished_ = false;
    bool finish_ok_ = true;
};

// Receive -> row-written latency in fixed log2 buckets (bucket b holds [2^(b-1), 2^b) ns).
class LatencyHistogram {
public:
    static constexpr std::size_t kBuckets = 64;

    void add(std::int64_t ns) noexcept;
    // Upper bound of the bucket holding the p-quantile (0 < p <= 1); 0 when empty.
    std::int64_t percentile(double p) const noexcept;
    std::uint64_t count() const noexcept { return count_; }

private:
    std::array<std::uint64_t, kBuckets> buckets_{};
    std::uint64_t count_ = 0;
};

struct ProcessorConfig {
    Venue venue = Venue::Spot;
    std::vector<SymbolSpec> symbols;
    bool trade_rows = true;
};

struct ProcessorStats {
    std::uint64_t md_rows = 0;
    std::uint64_t ob_rows = 0;
    std::uint64_t snapshot_rows = 0;
    std::uint64_t payloads_minified = 0;
    SymbolCounters totals;           // every per-symbol counter summed over symbols
    std::size_t retained_bytes = 0;  // processor and per-symbol storage (no ring, no network)
    LatencyHistogram latency;
};

// The processing thread's per-record logic (design §6.5): epoch rule, raw row, one parse,
// sync/book/cross-check, order-book row. Never allocates after construction.
class Processor {
public:
    Processor(ProcessorConfig config, std::vector<std::unique_ptr<SymbolOutput>> outputs,
              SnapshotRequester& requester);
    Processor(const Processor&) = delete;
    ~Processor() = default;
    Processor& operator=(const Processor&) = delete;
    Processor(Processor&&) = delete;
    Processor& operator=(Processor&&) = delete;

    void process(const RecordView& record) noexcept;

    // A writer failed; later records are ignored. Read on the processor thread only.
    bool fatal() const noexcept { return fatal_; }

    void flush() noexcept;
    bool finish() noexcept;  // flushes and closes every output; false on any failure

    const SymbolCounters& counters(std::size_t symbol) const { return symbols_[symbol]->counters; }
    ProcessorStats stats() const;

private:
    struct PerSymbol {
        PerSymbol(Venue venue, std::unique_ptr<SymbolOutput> out)
            : book(venue, counters), output(std::move(out)) {}
        SymbolCounters counters;
        SymbolBook book;
        std::unique_ptr<SymbolOutput> output;
        std::uint64_t next_seq = 0;
    };

    PaddedView minify_if_needed(PaddedView payload) noexcept;
    Emit process_market(PerSymbol& s, const SymbolSpec& spec, StreamKind kind,
                        PaddedView payload) noexcept;
    Emit process_snapshot(PerSymbol& s, PaddedView payload) noexcept;

    ProcessorConfig config_;
    std::vector<std::unique_ptr<PerSymbol>> symbols_;
    SnapshotRequester& requester_;
    PayloadParser parser_;
    DepthDiff diff_;
    Depth5 depth5_{};
    Trade trade_{};
    DepthSnapshot snapshot_;
    std::unique_ptr<char[]> minify_buffer_;  // NOLINT(modernize-avoid-c-arrays): dynamically sized
                                             // preallocated backing storage
    TopOfBook top_{};
    LatencyHistogram latency_;
    std::uint64_t md_rows_ = 0;
    std::uint64_t ob_rows_ = 0;
    std::uint64_t snapshot_rows_ = 0;
    std::uint64_t payloads_minified_ = 0;
    bool fatal_ = false;
};

// The §8 stats line ("stats uptime_s=… frames=… …"). `io` is null offline (replay), which omits
// the io keys; retained_kib adds the ring capacity and the receive buffers when given.
std::string format_stats(const ProcessorStats& stats, const IoMetrics* io,
                         std::size_t ring_capacity_bytes, std::size_t ring_hw_bytes,
                         std::int64_t uptime_s);

}  // namespace blc
