#include "blc/pipeline/processor.hpp"

#include "blc/core/time.hpp"
#include "blc/csv/schema.hpp"
#include "blc/pipeline/row_format.hpp"

#include <cmath>
#include <simdjson.h>

namespace blc {

namespace {

constexpr std::size_t kMinifyCapacity = kMaxPayloadBytes + simdjson::SIMDJSON_PADDING;

bool has_whitespace(const char* p, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const char c = p[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            return true;
        }
    }
    return false;
}

void accumulate(SymbolCounters& a, const SymbolCounters& b) noexcept {
    a.stale_diffs += b.stale_diffs;
    a.gaps += b.gaps;
    a.resets += b.resets;
    a.syncs += b.syncs;
    a.resyncs += b.resyncs;
    a.snapshot_too_old += b.snapshot_too_old;
    a.coverage_exhausted += b.coverage_exhausted;
    a.window_resets += b.window_resets;
    a.window_trimmed += b.window_trimmed;
    a.depth5_verified += b.depth5_verified;
    a.depth5_partially_verified += b.depth5_partially_verified;
    a.depth5_mismatch += b.depth5_mismatch;
    a.depth5_stale += b.depth5_stale;
    a.depth5_unchecked += b.depth5_unchecked;
    a.depth5_levels_unchecked += b.depth5_levels_unchecked;
    a.depth5_full_checks += b.depth5_full_checks;
    a.depth5_partial_checks += b.depth5_partial_checks;
    a.depth5_full_levels_compared += b.depth5_full_levels_compared;
    a.depth5_partial_levels_compared += b.depth5_partial_levels_compared;
    a.parse_errors += b.parse_errors;
    a.oversized_events += b.oversized_events;
    a.epoch_regressions += b.epoch_regressions;
    a.refresh_ok += b.refresh_ok;
    a.refresh_mismatch += b.refresh_mismatch;
}

}  // namespace

// --- FileOutput ---------------------------------------------------------------------------------

FileOutput::FileOutput(std::unique_ptr<SymbolFiles> files, Venue venue, std::string upper_symbol)
    : files_(std::move(files)), venue_(venue), symbol_(std::move(upper_symbol)) {}

void FileOutput::market_row(const RecordView& record, std::string_view payload_minified) noexcept {
    const RecordHeader& h = record.header;
    const SplitTime t = split_ns(h.recv_ns);
    CsvRow(files_->market_data())
        .integer(t.sec)
        .integer(t.nsec)
        .text(to_string(venue_))
        .text(to_string(h.stream))
        .unsigned_integer(h.shard)
        .unsigned_integer(h.epoch)
        .unsigned_integer(h.conn_seq)
        .text(symbol_)
        .text(payload_minified)
        .end();
    ++market_rows_;
}

void FileOutput::snapshot_row(const RecordView& record, std::size_t apply_before_row,
                              std::string_view payload_minified) noexcept {
    const RecordHeader& h = record.header;
    const SplitTime t = split_ns(h.recv_ns);
    CsvRow(files_->snapshots())
        .integer(t.sec)
        .integer(t.nsec)
        .text(to_string(venue_))
        .unsigned_integer(h.shard)
        .unsigned_integer(h.epoch)
        .text(symbol_)
        .unsigned_integer(apply_before_row)
        .text(payload_minified)
        .end();
    files_->end_snapshot_row();
}

void FileOutput::book_row(std::int64_t recv_ns, std::uint64_t seq, std::int32_t id, Emit emit,
                          const TopOfBook& top) noexcept {
    CsvRow row(files_->orderbook());
    write_book_row(row, recv_ns, seq, id, emit.type, emit.side, top);
}

// --- BookOnlyOutput -----------------------------------------------------------------------------

BookOnlyOutput::BookOnlyOutput(const std::filesystem::path& orderbook_path)
    : buffer_(std::make_unique<char[]>(SymbolFiles::kBufferBytes)),
      file_(OutputFile::create(orderbook_path, buffer_.get(), SymbolFiles::kBufferBytes)) {
    file_.write(kOrderBookHeader);
    file_.write_char('\n');
}

void BookOnlyOutput::book_row(std::int64_t recv_ns, std::uint64_t seq, std::int32_t id, Emit emit,
                              const TopOfBook& top) noexcept {
    CsvRow row(file_);
    write_book_row(row, recv_ns, seq, id, emit.type, emit.side, top);
}

bool BookOnlyOutput::flush() noexcept {
    return finished_ ? finish_ok_ : file_.flush();
}

bool BookOnlyOutput::finish() noexcept {
    if (!finished_) {
        finished_ = true;
        finish_ok_ = file_.close();
    }
    return finish_ok_;
}

// --- LatencyHistogram ---------------------------------------------------------------------------

void LatencyHistogram::add(std::int64_t ns) noexcept {
    std::size_t bucket = 0;
    if (ns > 0) {
        bucket = static_cast<std::size_t>(64 - __builtin_clzll(static_cast<std::uint64_t>(ns)));
        if (bucket >= kBuckets) {
            bucket = kBuckets - 1;
        }
    }
    ++buckets_[bucket];
    ++count_;
}

std::int64_t LatencyHistogram::percentile(double p) const noexcept {
    if (count_ == 0) {
        return 0;
    }
    if (p > 1.0) {
        p = 1.0;
    }
    auto target = static_cast<std::uint64_t>(std::ceil(p * static_cast<double>(count_)));
    if (target == 0) {
        target = 1;
    }
    std::uint64_t seen = 0;
    for (std::size_t b = 0; b < kBuckets; ++b) {
        seen += buckets_[b];
        if (seen >= target) {
            return b == 0 ? 0 : (b >= 63 ? INT64_MAX : std::int64_t{1} << b);
        }
    }
    return INT64_MAX;
}

// --- Processor ----------------------------------------------------------------------------------

Processor::Processor(ProcessorConfig config, std::vector<std::unique_ptr<SymbolOutput>> outputs,
                     SnapshotRequester& requester)
    : config_(std::move(config)),
      requester_(requester),
      diff_(PayloadParser::make_diff()),
      snapshot_(PayloadParser::make_snapshot()),
      minify_buffer_(std::make_unique<char[]>(kMinifyCapacity)) {
    symbols_.reserve(outputs.size());
    for (auto& out : outputs) {
        symbols_.push_back(std::make_unique<PerSymbol>(config_.venue, std::move(out)));
    }
}

PaddedView Processor::minify_if_needed(PaddedView payload) noexcept {
    if (payload.size > kMaxPayloadBytes || !has_whitespace(payload.data, payload.size)) {
        return payload;
    }
    std::size_t out_len = 0;
    if (simdjson::minify(payload.data, payload.size, minify_buffer_.get(), out_len) !=
            simdjson::SUCCESS ||
        out_len == payload.size) {
        return payload;  // invalid JSON is left for the parser to reject; or nothing removed
    }
    ++payloads_minified_;
    return PaddedView{minify_buffer_.get(), out_len, kMinifyCapacity};
}

Emit Processor::process_market(PerSymbol& s, const SymbolSpec& spec, StreamKind kind,
                               PaddedView payload) noexcept {
    switch (kind) {
        case StreamKind::DepthDiff: {
            const ParseStatus st = parser_.diff(config_.venue, payload, spec.upper, diff_);
            if (st == ParseStatus::Ok) {
                return s.book.on_diff(diff_);
            }
            return s.book.on_diff_error(st == ParseStatus::Oversized ? UnsyncCause::Oversized
                                                                     : UnsyncCause::DiffParseError);
        }
        case StreamKind::Depth5:
            if (parser_.depth5(config_.venue, payload, spec.upper, depth5_) == ParseStatus::Ok) {
                return s.book.on_depth5(depth5_);
            }
            break;
        case StreamKind::Trade:
            if (parser_.trade(config_.venue, payload, spec.upper, trade_) == ParseStatus::Ok) {
                return s.book.on_trade(trade_, config_.trade_rows);
            }
            break;
    }
    ++s.counters.parse_errors;  // depth5/trade: counted, no row, book unchanged (§5.7)
    return {};
}

Emit Processor::process_snapshot(PerSymbol& s, PaddedView payload) noexcept {
    const ParseStatus st = parser_.snapshot(config_.venue, payload, snapshot_);
    if (st == ParseStatus::Ok) {
        return s.book.on_snapshot(snapshot_);
    }
    return s.book.on_snapshot_error(st == ParseStatus::Oversized);
}

void Processor::process(const RecordView& record) noexcept {
    if (fatal_) {
        return;
    }
    const RecordHeader& h = record.header;
    if (h.symbol >= symbols_.size()) {
        return;  // cannot happen: the router and replay only produce configured symbols
    }
    PerSymbol& s = *symbols_[h.symbol];
    const SymbolSpec& spec = config_.symbols[h.symbol];

    // 1. Per-symbol epoch rule (§5.7).
    switch (s.book.observe_epoch(h.epoch)) {
        case SymbolBook::EpochCheck::Regression:
            return;
        case SymbolBook::EpochCheck::Reset:
            s.book.enter_unsynced(UnsyncCause::NewEpoch);
            break;
        case SymbolBook::EpochCheck::Same:
            break;
    }

    // 2. The raw row first, then 3-4. one parse and the book logic.
    const PaddedView payload = minify_if_needed(record.payload);
    const std::string_view text(payload.data, payload.size);
    Emit emit;
    if (h.kind == RecordKind::Market) {
        s.output->market_row(record, text);
        ++md_rows_;
        emit = process_market(s, spec, h.stream, payload);
    } else {
        s.output->snapshot_row(record, s.output->market_rows(), text);
        ++snapshot_rows_;
        emit = process_snapshot(s, payload);
    }

    // 5. The order-book row (§5.8) and any snapshot request.
    if (emit.row) {
        s.book.book().top5(top_);
        s.output->book_row(h.recv_ns, s.next_seq++, spec.id, emit, top_);
        ++ob_rows_;
    }
    if (s.book.snapshot_wanted()) {
        requester_.request(h.symbol, h.epoch);
        s.book.mark_requested();
    }
    if (s.output->failed()) {
        fatal_ = true;
        return;
    }
    if (h.steady_ns != 0) {
        latency_.add(steady_clock_ns() - h.steady_ns);
    }
}

void Processor::flush() noexcept {
    for (auto& s : symbols_) {
        if (!s->output->flush()) {
            fatal_ = true;
        }
    }
}

bool Processor::finish() noexcept {
    bool ok = true;
    for (auto& s : symbols_) {
        if (!s->output->finish()) {
            ok = false;
        }
    }
    if (!ok) {
        fatal_ = true;
    }
    return ok;
}

ProcessorStats Processor::stats() const {
    ProcessorStats st;
    st.md_rows = md_rows_;
    st.ob_rows = ob_rows_;
    st.snapshot_rows = snapshot_rows_;
    st.payloads_minified = payloads_minified_;
    std::size_t retained = parser_.retained_bytes() + kMinifyCapacity +
                           (diff_.bids.capacity() + diff_.asks.capacity() +
                            snapshot_.bids.capacity() + snapshot_.asks.capacity()) *
                               sizeof(Level);
    for (const auto& s : symbols_) {
        accumulate(st.totals, s->counters);
        retained += s->book.retained_bytes() + s->output->retained_bytes();
    }
    st.retained_bytes = retained;
    st.latency = latency_;
    return st;
}

// --- Stats line ---------------------------------------------------------------------------------

std::string format_stats(const ProcessorStats& s, const IoMetrics* io,
                         std::size_t ring_capacity_bytes, std::size_t ring_hw_bytes,
                         std::int64_t uptime_s) {
    std::string line = "stats";
    const auto kv = [&line](const char* key, std::uint64_t value) {
        line += ' ';
        line += key;
        line += '=';
        line += std::to_string(value);
    };
    const auto load = [](const IoMetrics::Counter& c) { return c.load(std::memory_order_relaxed); };
    const SymbolCounters& c = s.totals;
    line += " uptime_s=" + std::to_string(uptime_s);
    if (io != nullptr) {
        kv("frames", load(io->frames_received));
    }
    kv("md_rows", s.md_rows);
    kv("ob_rows", s.ob_rows);
    kv("snapshot_rows", s.snapshot_rows);
    kv("parse_errors", c.parse_errors);
    kv("oversized_events", c.oversized_events);
    kv("stale_diffs", c.stale_diffs);
    kv("gaps", c.gaps);
    kv("resets", c.resets);
    kv("syncs", c.syncs);
    kv("resyncs", c.resyncs);
    if (io != nullptr) {
        kv("reconnects", load(io->reconnects));
    }
    kv("coverage_exhausted", c.coverage_exhausted);
    kv("snapshot_too_old", c.snapshot_too_old);
    kv("epoch_regressions", c.epoch_regressions);
    kv("window_resets", c.window_resets);
    kv("window_trimmed", c.window_trimmed);
    kv("refresh_ok", c.refresh_ok);
    kv("refresh_mismatch", c.refresh_mismatch);
    kv("depth5_verified", c.depth5_verified);
    kv("depth5_partially_verified", c.depth5_partially_verified);
    kv("depth5_mismatch", c.depth5_mismatch);
    kv("depth5_stale", c.depth5_stale);
    kv("depth5_unchecked", c.depth5_unchecked);
    kv("depth5_levels_unchecked", c.depth5_levels_unchecked);
    kv("depth5_full_checks", c.depth5_full_checks);
    kv("depth5_partial_checks", c.depth5_partial_checks);
    kv("depth5_full_levels_compared", c.depth5_full_levels_compared);
    kv("depth5_partial_levels_compared", c.depth5_partial_levels_compared);
    kv("payloads_minified", s.payloads_minified);
    std::size_t retained = s.retained_bytes + ring_capacity_bytes;
    if (io != nullptr) {
        kv("bytes", load(io->bytes_received));
        kv("malformed_frames", load(io->malformed_frames));
        kv("ring_drops", load(io->ring_drops));
        kv("shard_losses", load(io->shard_losses));
        kv("snapshots_discarded", load(io->snapshots_discarded));
        kv("connects", load(io->connects));
        kv("connect_failures", load(io->connect_failures));
        kv("connect_throttled", load(io->connect_throttled));
        kv("rest_requests", load(io->rest_requests));
        kv("rest_failures", load(io->rest_failures));
        kv("rest_cooldowns", load(io->rest_cooldowns));
        kv("rest_weight_used", load(io->rest_weight_used));
        retained += static_cast<std::size_t>(load(io->receive_capacity_bytes));
    }
    kv("ring_hw_kib", ring_hw_bytes / 1024);
    kv("retained_kib", retained / 1024);
    kv("lat_p50_us", static_cast<std::uint64_t>(s.latency.percentile(0.50) / 1000));
    kv("lat_p99_us", static_cast<std::uint64_t>(s.latency.percentile(0.99) / 1000));
    return line;
}

}  // namespace blc
