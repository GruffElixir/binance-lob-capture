#include "blc/verify/verify.hpp"

#include "blc/csv/csv_reader.hpp"
#include "blc/csv/rows.hpp"
#include "blc/csv/schema.hpp"
#include "blc/parse/json_workspace.hpp"

#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <system_error>

namespace blc {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxDiagnostics = 100;

class Diagnostics {
public:
    void add(const std::string& message) {
        ++total_;
        if (errors_.size() < kMaxDiagnostics) {
            errors_.push_back(message);
        }
    }
    VerifyResult result() const {
        VerifyResult r;
        r.ok = total_ == 0;
        r.errors = errors_;
        if (total_ > kMaxDiagnostics) {
            r.errors.push_back(std::to_string(total_ - kMaxDiagnostics) +
                               " more diagnostics omitted");
        }
        return r;
    }

private:
    std::vector<std::string> errors_;
    std::size_t total_ = 0;
};

enum class FileKind { Unknown, Market, OrderBook, Sidecar };

std::string join(const std::vector<std::string_view>& fields) {
    std::string s;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        s += i != 0u ? "," : "";
        s.append(fields[i]);
    }
    return s;
}

std::string where(const fs::path& p, std::size_t line) {
    return p.string() + ":" + std::to_string(line) + ": ";
}

fs::path canonical_or_self(const fs::path& p) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p : c;
}

struct MarketFile {
    fs::path path;
    fs::path canonical;
    std::uint64_t rows = 0;
    std::optional<std::uint16_t> shard;
};

struct SidecarCheck {
    fs::path path;
    fs::path paired_market;  // canonical
    std::uint64_t max_apply = 0;
};

class Verifier {
public:
    explicit Verifier(const VerifyOptions& options) : options_(options) {}

    VerifyResult run() {
        for (const fs::path& f : options_.files) {
            check_file(f);
        }
        for (const SidecarCheck& s : sidecars_) {
            for (const MarketFile& m : markets_) {
                if (m.canonical == s.paired_market && s.max_apply > m.rows) {
                    diag_.add(s.path.string() + ": apply_before_row " +
                              std::to_string(s.max_apply) + " is beyond the " +
                              std::to_string(m.rows) + " rows of " + m.path.string());
                }
            }
        }
        if (options_.epoch_log) {
            check_epoch_log(*options_.epoch_log);
        }
        return diag_.result();
    }

private:
    void check_file(const fs::path& path) {
        std::unique_ptr<CsvReader> reader;
        try {
            reader = std::make_unique<CsvReader>(CsvReader::open(path));
        } catch (const std::exception& e) {
            diag_.add(path.string() + ": " + e.what());
            return;
        }
        std::vector<std::string_view> fields;
        try {
            if (!reader->next(fields)) {
                diag_.add(path.string() + ": empty file (no header)");
                return;
            }
            const std::string header = join(fields);
            if (header == kMarketDataHeader) {
                check_market(path, *reader);
            } else if (header == kOrderBookHeader) {
                check_orderbook(path, *reader);
            } else if (header == kSnapshotHeader) {
                check_sidecar(path, *reader);
            } else {
                diag_.add(path.string() + ": header is not a known CSV header");
            }
        } catch (const CsvFormatError& e) {
            diag_.add(where(path, e.line) + e.what());
        } catch (const std::exception& e) {
            diag_.add(path.string() + ": " + e.what());
        }
    }

    void check_market(const fs::path& path, CsvReader& reader) {
        MarketFile file{path, canonical_or_self(path), 0, std::nullopt};
        std::vector<std::string_view> f;
        MarketRow row;
        std::string symbol;
        bool have_last = false;
        std::uint32_t last_epoch = 0;
        std::uint64_t last_seq = 0;
        while (reader.next(f)) {
            const std::string at = where(path, reader.line());
            ++file.rows;
            if (const char* e = parse_market_row(f, row)) {
                diag_.add(at + e);
                continue;
            }
            if (!json_.object(row.payload)) {
                diag_.add(at + "payload_json is not a valid JSON object");
            }
            if (symbol.empty()) {
                symbol = std::string(row.symbol);
            } else if (row.symbol != symbol) {
                diag_.add(at + "symbol changes within the file");
            }
            if (!file.shard) {
                file.shard = row.header.shard;
            } else if (*file.shard != row.header.shard) {
                diag_.add(at + "shard_id changes within the file");
            }
            if (have_last) {
                if (row.header.epoch < last_epoch) {
                    diag_.add(at + "conn_epoch decreases");
                } else if (row.header.epoch == last_epoch && row.header.conn_seq <= last_seq) {
                    diag_.add(at + "conn_seq does not increase within its epoch");
                }
            }
            have_last = true;
            last_epoch = row.header.epoch;
            last_seq = row.header.conn_seq;
        }
        markets_.push_back(file);
    }

    template <class Less>
    void check_side(const std::string& at, const std::vector<std::string_view>& f,
                    std::size_t price_col, const char* name, Less better) {
        bool padding = false;
        std::int64_t previous = 0;
        for (std::size_t i = 0; i < 5; ++i) {
            std::int64_t price = 0;
            std::int64_t size = 0;
            const bool ok_p = parse_signed_field(f[price_col + i], 0,
                                                 std::numeric_limits<std::int64_t>::max(), price);
            const bool ok_s = parse_signed_field(f[price_col + 5 + i], 0,
                                                 std::numeric_limits<std::int64_t>::max(), size);
            if (!ok_p || !ok_s) {
                diag_.add(at + name + " level " + std::to_string(i) +
                          ": price/size is not a non-negative int64");
                return;
            }
            if (price == 0 && size == 0) {
                padding = true;
                continue;
            }
            if (padding) {
                diag_.add(at + name + " level " + std::to_string(i) + " follows zero padding");
                return;
            }
            if (price == 0 || size == 0) {
                diag_.add(at + name + " level " + std::to_string(i) +
                          ": price and size must both be positive");
                return;
            }
            if (i > 0 && !better(previous, price)) {
                diag_.add(at + name + " prices are not strictly ordered best first");
                return;
            }
            previous = price;
        }
    }

    void check_orderbook(const fs::path& path, CsvReader& reader) {
        std::vector<std::string_view> f;
        std::uint64_t expected_seq = 0;
        std::optional<std::int64_t> id;
        while (reader.next(f)) {
            const std::string at = where(path, reader.line());
            const std::uint64_t index = expected_seq++;
            if (f.size() != kOrderBookColumns) {
                diag_.add(at + "wrong number of columns");
                continue;
            }
            std::int64_t sec = 0;
            std::int64_t nsec = 0;
            std::int64_t ns = 0;
            if (!parse_signed_field(f[0], std::numeric_limits<std::int64_t>::min(),
                                    std::numeric_limits<std::int64_t>::max(), sec) ||
                !parse_signed_field(f[1], 0, 999'999'999, nsec) || !join_recv_ns(sec, nsec, ns)) {
                diag_.add(at + "tsec/tnsec invalid");
            }
            std::uint64_t seq = 0;
            if (!parse_unsigned_field(f[2], std::numeric_limits<std::uint64_t>::max(), seq)) {
                diag_.add(at + "seqNo is not a uint64");
            } else if (seq != index) {
                diag_.add(at + "seqNo " + std::to_string(seq) + " (expected " +
                          std::to_string(index) + ")");
            }
            std::int64_t row_id = 0;
            if (!parse_signed_field(f[3], std::numeric_limits<std::int32_t>::min(),
                                    std::numeric_limits<std::int32_t>::max(), row_id)) {
                diag_.add(at + "id is not an int32");
            } else if (!id) {
                id = row_id;
            } else if (*id != row_id) {
                diag_.add(at + "id changes within the file");
            }
            const std::string_view type = f[4];
            const std::string_view side = f[5];
            const bool type_ok = type == "R" || type == "D" || type == "P" || type == "T";
            const bool side_ok = side == "B" || side == "S" || side == "N";
            if (!type_ok) {
                diag_.add(at + "type is not R, D, P or T");
            }
            if (!side_ok) {
                diag_.add(at + "side is not B, S or N");
            }
            if (type_ok && side_ok &&
                (((type == "R" || type == "P") && side != "N") || (type == "T" && side == "N"))) {
                diag_.add(at + "side does not fit the row type");
            }
            if (index == 0 && type != "R") {
                diag_.add(at + "the first row is not an R row");
            }
            check_side(at, f, 6, "bid", [](std::int64_t a, std::int64_t b) { return a > b; });
            check_side(at, f, 16, "ask", [](std::int64_t a, std::int64_t b) { return a < b; });
        }
    }

    void check_sidecar(const fs::path& path, CsvReader& reader) {
        SidecarCheck check;
        check.path = path;
        const std::string name = path.filename().string();
        const std::string prefix = "depth_snapshots_";
        if (name.rfind(prefix, 0) == 0) {
            check.paired_market = canonical_or_self(path.parent_path() /
                                                    ("market_data_" + name.substr(prefix.size())));
        }
        std::vector<std::string_view> f;
        SidecarRow row;
        std::string symbol;
        bool first = true;
        while (reader.next(f)) {
            const std::string at = where(path, reader.line());
            if (const char* e = parse_sidecar_row(f, row)) {
                diag_.add(at + e);
                continue;
            }
            if (!json_.object(row.payload)) {
                diag_.add(at + "payload_json is not a valid JSON object");
            }
            if (symbol.empty()) {
                symbol = std::string(row.symbol);
            } else if (row.symbol != symbol) {
                diag_.add(at + "symbol changes within the file");
            }
            if (!first && row.apply_before_row < check.max_apply) {
                diag_.add(at + "apply_before_row decreases");
            }
            if (first || row.apply_before_row > check.max_apply) {
                check.max_apply = row.apply_before_row;
            }
            first = false;
        }
        sidecars_.push_back(check);
    }

    // --- Frame accounting per (shard, epoch) against the epoch_closed log lines -----------------

    struct LogEntry {
        std::uint32_t epoch = 0;
        std::uint64_t frames = 0;
        std::uint64_t malformed = 0;
        std::uint64_t drops = 0;
    };

    static bool field_value(const std::string& line, const char* key, std::uint64_t& out) {
        const std::string k = std::string(" ") + key + "=";
        const std::size_t at = line.find(k);
        if (at == std::string::npos) {
            return false;
        }
        const std::size_t start = at + k.size();
        std::size_t end = line.find(' ', start);
        if (end == std::string::npos) {
            end = line.size();
        }
        return parse_unsigned_field(std::string_view(line).substr(start, end - start),
                                    std::numeric_limits<std::uint64_t>::max(), out);
    }

    // Reads the shard's next epoch_closed line; epochs must strictly increase.
    class LogCursor {
    public:
        LogCursor(const fs::path& path, std::uint16_t shard, Diagnostics& diag)
            : in_(path), shard_(shard), diag_(diag) {}
        bool good() const { return static_cast<bool>(in_); }

        bool next(LogEntry& out) {
            std::string line;
            while (std::getline(in_, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                const std::size_t at = line.find("epoch_closed ");
                if (at == std::string::npos) {
                    continue;
                }
                const std::string rest = line.substr(at + 12);
                std::uint64_t shard = 0;
                std::uint64_t epoch = 0;
                LogEntry e;
                if (!field_value(rest, "shard", shard) || !field_value(rest, "epoch", epoch) ||
                    !field_value(rest, "frames", e.frames) ||
                    !field_value(rest, "malformed", e.malformed) ||
                    !field_value(rest, "drops", e.drops) ||
                    epoch > std::numeric_limits<std::uint32_t>::max()) {
                    diag_.add("epoch log: malformed epoch_closed line: " + line);
                    continue;
                }
                if (shard != shard_) {
                    continue;
                }
                e.epoch = static_cast<std::uint32_t>(epoch);
                if (have_last_ && e.epoch <= last_epoch_) {
                    diag_.add("epoch log: duplicate or out-of-order epoch_closed line for shard=" +
                              std::to_string(shard_) + " epoch=" + std::to_string(e.epoch));
                    continue;
                }
                have_last_ = true;
                last_epoch_ = e.epoch;
                out = e;
                return true;
            }
            return false;
        }

    private:
        std::ifstream in_;
        std::uint16_t shard_;
        Diagnostics& diag_;
        bool have_last_ = false;
        std::uint32_t last_epoch_ = 0;
    };

    // One market-data file of the shard, read row by row in (epoch, conn_seq) order.
    struct RowCursor {
        std::unique_ptr<CsvReader> reader;
        std::vector<std::string_view> fields;
        bool valid = false;
        std::uint32_t epoch = 0;
        std::uint64_t seq = 0;

        void advance() {
            valid = false;
            MarketRow row;
            while (reader->next(fields)) {
                if (parse_market_row(fields, row) == nullptr) {
                    epoch = row.header.epoch;
                    seq = row.header.conn_seq;
                    valid = true;
                    return;
                }
            }
        }
    };

    void check_epoch_log(const fs::path& log) {
        std::vector<std::uint16_t> shards;
        for (const MarketFile& m : markets_) {
            if (m.shard) {
                bool seen = false;
                for (const std::uint16_t s : shards) {
                    seen = seen || s == *m.shard;
                }
                if (!seen) {
                    shards.push_back(*m.shard);
                }
            }
        }
        for (const std::uint16_t shard : shards) {
            try {
                check_shard(log, shard);
            } catch (const std::exception& e) {
                diag_.add("epoch log check for shard=" + std::to_string(shard) + ": " + e.what());
            }
        }
    }

    void check_shard(const fs::path& log, std::uint16_t shard) {
        LogCursor entries(log, shard, diag_);
        if (!entries.good()) {
            diag_.add("cannot read epoch log " + log.string());
            return;
        }
        std::vector<RowCursor> cursors;
        for (const MarketFile& m : markets_) {
            if (m.shard && *m.shard == shard) {
                RowCursor c;
                c.reader = std::make_unique<CsvReader>(CsvReader::open(m.path));
                c.reader->next(c.fields);  // header
                c.advance();
                cursors.push_back(std::move(c));
            }
        }
        const auto smallest = [&]() -> RowCursor* {
            RowCursor* best = nullptr;
            for (RowCursor& c : cursors) {
                if (c.valid && (best == nullptr || c.epoch < best->epoch ||
                                (c.epoch == best->epoch && c.seq < best->seq))) {
                    best = &c;
                }
            }
            return best;
        };
        const std::string tag = "shard=" + std::to_string(shard) + " epoch=";
        LogEntry entry;
        bool have_entry = entries.next(entry);
        RowCursor* row = smallest();
        while (have_entry || row != nullptr) {
            if (row != nullptr && (!have_entry || row->epoch < entry.epoch)) {
                const std::uint32_t epoch = row->epoch;
                diag_.add(tag + std::to_string(epoch) +
                          ": market-data rows but no epoch_closed line");
                while (row != nullptr && row->epoch == epoch) {
                    row->advance();
                    row = smallest();
                }
                continue;
            }
            std::uint64_t rows = 0;
            bool have_seq = false;
            std::uint64_t last_seq = 0;
            while (row != nullptr && row->epoch == entry.epoch) {
                if (have_seq && row->seq == last_seq) {
                    diag_.add(tag + std::to_string(entry.epoch) + ": duplicate conn_seq " +
                              std::to_string(row->seq) + " across market-data files");
                } else {
                    ++rows;
                }
                if (row->seq >= entry.frames) {
                    diag_.add(tag + std::to_string(entry.epoch) + ": conn_seq " +
                              std::to_string(row->seq) + " is outside the logged " +
                              std::to_string(entry.frames) + " frames");
                }
                have_seq = true;
                last_seq = row->seq;
                row->advance();
                row = smallest();
            }
            if (rows + entry.malformed + entry.drops != entry.frames) {
                diag_.add(tag + std::to_string(entry.epoch) + ": " + std::to_string(rows) +
                          " rows + " + std::to_string(entry.malformed) + " malformed + " +
                          std::to_string(entry.drops) +
                          " drops != " + std::to_string(entry.frames) + " frames");
            }
            have_entry = entries.next(entry);
        }
    }

    const VerifyOptions& options_;
    Diagnostics diag_;
    JsonWorkspace json_;
    std::vector<MarketFile> markets_;
    std::vector<SidecarCheck> sidecars_;
};

}  // namespace

VerifyResult verify_files(const VerifyOptions& options) {
    return Verifier(options).run();
}

}  // namespace blc
