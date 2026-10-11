#include "blc/replay/replay.hpp"

#include "blc/core/errors.hpp"
#include "blc/core/symbols.hpp"
#include "blc/core/time.hpp"
#include "blc/csv/csv_reader.hpp"
#include "blc/csv/rows.hpp"
#include "blc/csv/schema.hpp"
#include "blc/parse/payload_parser.hpp"
#include "blc/pipeline/output_files.hpp"
#include "blc/pipeline/processor.hpp"
#include "blc/pipeline/snapshot_requester.hpp"
#include "blc/util/logging.hpp"

#include <cstring>
#include <optional>
#include <simdjson.h>
#include <string>
#include <system_error>
#include <vector>

namespace blc {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kMarketPrefix = "market_data_";
constexpr std::string_view kCsvSuffix = ".csv";
constexpr std::string_view kOrderBookSuffix = "_orderbook.csv";

struct Input {
    fs::path path;
    fs::path canonical;
    Venue venue = Venue::Spot;
    std::string symbol;
    std::string date;
    fs::path sidecar;
    std::string output_name;
};

// Thrown inside one file's replay; the file fails, the others continue.
struct FileError {
    std::string message;
};

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// market_data_<venue>_<SYMBOL>_<YYYY-MM-DD>.csv
bool parse_file_name(std::string_view name, Input& in) {
    if (name.substr(0, kMarketPrefix.size()) != kMarketPrefix || !ends_with(name, kCsvSuffix)) {
        return false;
    }
    std::string_view rest = name.substr(kMarketPrefix.size());
    rest.remove_suffix(kCsvSuffix.size());
    const std::size_t venue_end = rest.find('_');
    if (venue_end == std::string_view::npos || rest.size() < venue_end + 1 + 2 + 11) {
        return false;
    }
    const auto venue = parse_venue(rest.substr(0, venue_end));
    const std::string_view date = rest.substr(rest.size() - 10);
    const std::string_view symbol = rest.substr(venue_end + 1, rest.size() - venue_end - 1 - 11);
    if (!venue || rest[rest.size() - 11] != '_' || !valid_symbol(symbol)) {
        return false;
    }
    for (std::size_t i = 0; i < date.size(); ++i) {
        const bool dash = i == 4 || i == 7;
        if (dash ? date[i] != '-' : (date[i] < '0' || date[i] > '9')) {
            return false;
        }
    }
    in.venue = *venue;
    in.symbol = std::string(symbol);
    in.date = std::string(date);
    return true;
}

std::string read_header(const fs::path& path) {
    CsvReader reader = CsvReader::open(path);
    std::vector<std::string_view> fields;
    if (!reader.next(fields)) {
        return {};
    }
    std::string header;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        header += i != 0u ? "," : "";
        header.append(fields[i]);
    }
    return header;
}

void fail(const fs::path& path, const std::string& what) {
    log(LogLevel::Error, "replay " + path.string() + ": " + what);
}

class FileReplay {
public:
    FileReplay(const Input& in, const fs::path& output, const ReplayOptions& options)
        : in_(in),
          options_(options),
          spec_(parse_symbol_list(in.venue, in.symbol).at(0)),
          payload_(kMaxPayloadBytes + simdjson::SIMDJSON_PADDING),
          snapshot_payload_(kMaxPayloadBytes + simdjson::SIMDJSON_PADDING) {
        std::vector<std::unique_ptr<SymbolOutput>> outputs;
        outputs.push_back(std::make_unique<BookOnlyOutput>(output));
        processor_ = std::make_unique<Processor>(
            ProcessorConfig{in.venue, {spec_}, options.trade_rows}, std::move(outputs), requester_);
    }

    void run() {
        CsvReader md = CsvReader::open(in_.path);
        std::vector<std::string_view> fields;
        if (!md.next(fields) || !is_header(fields, kMarketDataHeader)) {
            throw FileError{"header is not the market-data header"};
        }
        if (!fs::exists(in_.sidecar)) {
            throw FileError{"missing sidecar " + in_.sidecar.string()};
        }
        CsvReader sc = CsvReader::open(in_.sidecar);
        sidecar_ = &sc;
        if (!sc.next(fields) || !is_header(fields, kSnapshotHeader)) {
            throw FileError{"sidecar " + in_.sidecar.string() +
                            ": header is not the depth-snapshot header"};
        }
        read_sidecar();

        const std::int64_t started = steady_clock_ns();
        std::int64_t next_stats = started + stats_interval_ns();
        MarketRow row;
        while (true) {
            const std::int64_t record_started =
                options_.record_latency_observer ? steady_clock_ns() : 0;
            bool more = false;
            try {
                more = md.next(fields);
            } catch (const CsvFormatError& e) {
                throw FileError{"line " + std::to_string(e.line) + ": " + e.what()};
            }
            if (!more) {
                break;
            }
            inject_snapshots_before(rows_);
            if (const char* e = parse_market_row(fields, row)) {
                throw FileError{"line " + std::to_string(md.line()) + ": " + e};
            }
            if (row.venue != in_.venue || row.symbol != in_.symbol) {
                throw FileError{"line " + std::to_string(md.line()) +
                                ": venue/symbol differ from the file name"};
            }
            process(row.header, row.payload, payload_);
            ++rows_;
            if (options_.record_latency_observer) {
                options_.record_latency_observer(steady_clock_ns() - record_started);
            }
            if ((rows_ & 4095) == 0 && stats_interval_ns() > 0 && steady_clock_ns() >= next_stats) {
                log(LogLevel::Info, format_stats(processor_->stats(), nullptr, 0, 0,
                                                 (steady_clock_ns() - started) / kNanosPerSecond));
                next_stats += stats_interval_ns();
            }
        }
        inject_snapshots_before(rows_);
        if (have_snapshot_) {
            throw FileError{"sidecar " + in_.sidecar.string() + " line " +
                            std::to_string(sidecar_line_) + ": apply_before_row " +
                            std::to_string(snapshot_.apply_before_row) + " is beyond the " +
                            std::to_string(rows_) + " market-data rows"};
        }
        if (!processor_->finish()) {
            throw FileError{"writing the order book failed"};
        }
        std::string summary = format_stats(processor_->stats(), nullptr, 0, 0,
                                           (steady_clock_ns() - started) / kNanosPerSecond);
        summary.replace(0, 5, "summary");
        log(LogLevel::Info, "replay " + in_.path.string() + ": " + summary);
    }

    void abandon() noexcept { processor_->finish(); }

private:
    static bool is_header(const std::vector<std::string_view>& fields, std::string_view header) {
        std::size_t pos = 0;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) {
                if (pos >= header.size() || header[pos] != ',') {
                    return false;
                }
                ++pos;
            }
            if (header.substr(pos, fields[i].size()) != fields[i]) {
                return false;
            }
            pos += fields[i].size();
        }
        return pos == header.size();
    }

    std::int64_t stats_interval_ns() const {
        return static_cast<std::int64_t>(options_.stats_interval_s) * kNanosPerSecond;
    }

    // Reads the next sidecar row into snapshot_ / snapshot_payload_ (copied: the reader's views
    // do not survive its next call).
    void read_sidecar() {
        std::vector<std::string_view> f;
        bool more = false;
        try {
            more = sidecar_->next(f);
        } catch (const CsvFormatError& e) {
            throw FileError{"sidecar " + in_.sidecar.string() + " line " + std::to_string(e.line) +
                            ": " + e.what()};
        }
        if (!more) {
            have_snapshot_ = false;
            return;
        }
        sidecar_line_ = sidecar_->line();
        const std::size_t previous = have_snapshot_ ? snapshot_.apply_before_row : 0;
        if (const char* e = parse_sidecar_row(f, snapshot_)) {
            throw FileError{"sidecar " + in_.sidecar.string() + " line " +
                            std::to_string(sidecar_line_) + ": " + e};
        }
        if (snapshot_.venue != in_.venue || snapshot_.symbol != in_.symbol) {
            throw FileError{"sidecar " + in_.sidecar.string() + " line " +
                            std::to_string(sidecar_line_) +
                            ": venue/symbol differ from the file name"};
        }
        if (snapshot_.apply_before_row < previous) {
            throw FileError{"sidecar " + in_.sidecar.string() + " line " +
                            std::to_string(sidecar_line_) + ": apply_before_row decreases"};
        }
        copy_payload(snapshot_.payload, snapshot_payload_);
        snapshot_.payload = std::string_view(snapshot_payload_.data(), snapshot_.payload.size());
        have_snapshot_ = true;
    }

    void inject_snapshots_before(std::uint64_t row) {
        while (have_snapshot_ && snapshot_.apply_before_row == row) {
            RecordView const v{snapshot_.header,
                               PaddedView{snapshot_payload_.data(), snapshot_.payload.size(),
                                          snapshot_payload_.size()}};
            processor_->process(v);
            check_fatal();
            read_sidecar();
        }
    }

    void copy_payload(std::string_view payload, std::vector<char>& buffer) const {
        if (payload.size() > kMaxPayloadBytes) {
            throw FileError{"payload larger than 1 MiB"};
        }
        std::memcpy(buffer.data(), payload.data(), payload.size());
        std::memset(buffer.data() + payload.size(), 0, simdjson::SIMDJSON_PADDING);
    }

    void process(const RecordHeader& header, std::string_view payload, std::vector<char>& buffer) {
        copy_payload(payload, buffer);
        processor_->process(
            RecordView{header, PaddedView{buffer.data(), payload.size(), buffer.size()}});
        check_fatal();
    }

    void check_fatal() const {
        if (processor_->fatal()) {
            throw FileError{"writing the order book failed"};
        }
    }

    const Input& in_;
    const ReplayOptions& options_;
    SymbolSpec spec_;
    NullRequester requester_;
    std::unique_ptr<Processor> processor_;
    std::vector<char> payload_;
    std::vector<char> snapshot_payload_;
    CsvReader* sidecar_ = nullptr;
    SidecarRow snapshot_;
    bool have_snapshot_ = false;
    std::size_t sidecar_line_ = 0;
    std::uint64_t rows_ = 0;
};

// Replays one input into `output`; false (after logging) on failure, with no output left behind.
bool replay_file(const Input& in, const fs::path& output, const ReplayOptions& options) {
    std::optional<FileReplay> replay;
    try {
        replay.emplace(in, output, options);
        replay->run();
        return true;
    } catch (const FileError& e) {
        fail(in.path, e.message);
    } catch (const std::exception& e) {
        fail(in.path, e.what());
    }
    if (replay) {
        replay->abandon();
    }
    std::error_code ec;
    fs::remove(output, ec);
    return false;
}

}  // namespace

int run_replay(const ReplayOptions& options) {
    bool any_failed = false;
    std::size_t market_inputs = 0;
    std::vector<Input> inputs;

    // Preflight: nothing below touches the output directory until every input is classified.
    for (const fs::path& path : options.inputs) {
        const std::string name = path.filename().string();
        if (ends_with(name, kOrderBookSuffix)) {
            log(LogLevel::Info, "replay: skipping order-book file " + path.string());
            continue;
        }
        Input in;
        in.path = path;
        std::error_code ec;
        in.canonical = fs::weakly_canonical(fs::absolute(path, ec), ec);
        if (ec || !fs::is_regular_file(path, ec)) {
            ++market_inputs;
            any_failed = true;
            fail(path, "cannot open the file");
            continue;
        }
        std::string header;
        try {
            header = read_header(path);
        } catch (const std::exception& e) {
            ++market_inputs;
            any_failed = true;
            fail(path, e.what());
            continue;
        }
        if (header == kOrderBookHeader) {
            log(LogLevel::Info, "replay: skipping order-book file " + path.string());
            continue;
        }
        ++market_inputs;
        if (header != kMarketDataHeader) {
            any_failed = true;
            fail(path, "header is not the market-data header");
            continue;
        }
        if (!parse_file_name(name, in)) {
            any_failed = true;
            fail(path, "name is not market_data_<venue>_<SYMBOL>_<YYYY-MM-DD>.csv");
            continue;
        }
        bool duplicate = false;
        for (const Input& other : inputs) {
            if (other.canonical == in.canonical) {
                duplicate = true;
            }
        }
        if (duplicate) {
            continue;  // the same file named twice is replayed once
        }
        in.sidecar = path.parent_path() / ("depth_snapshots_" + std::string(to_string(in.venue)) +
                                           "_" + in.symbol + "_" + in.date + ".csv");
        in.output_name = name.substr(0, name.size() - kCsvSuffix.size()) + "_orderbook.csv";
        inputs.push_back(std::move(in));
    }
    if (market_inputs == 0) {
        log(LogLevel::Error, "replay: no market-data file among the inputs");
        return 2;
    }
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        for (std::size_t j = i + 1; j < inputs.size(); ++j) {
            if (inputs[i].output_name == inputs[j].output_name) {
                log(LogLevel::Error, "replay: " + inputs[i].path.string() + " and " +
                                         inputs[j].path.string() + " would both write " +
                                         inputs[i].output_name);
                return 2;
            }
        }
    }

    // Output: create the directory and archive the planned set once.
    fs::path out_dir;
    try {
        out_dir = prepare_output_dir(options.output_dir);
        std::vector<std::string> names;
        names.reserve(inputs.size());
        for (const Input& in : inputs) {
            names.push_back(in.output_name);
        }
        if (const auto archive =
                archive_existing(out_dir, names, utc_compact_stamp(wall_clock_ns()))) {
            log(LogLevel::Warn, "replay: existing output moved to " + archive->string());
        }
    } catch (const IoError& e) {
        log(LogLevel::Error, std::string("replay: ") + e.what());
        return 1;
    }

    for (const Input& in : inputs) {
        if (!replay_file(in, out_dir / in.output_name, options)) {
            any_failed = true;
        }
    }
    return any_failed ? 1 : 0;
}

}  // namespace blc
