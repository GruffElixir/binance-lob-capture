#pragma once

#include "blc/core/types.hpp"
#include "blc/csv/csv_writer.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace blc {

// The three per-symbol file names of one run (design §3.1).
struct FileNames {
    std::string market_data;
    std::string orderbook;
    std::string snapshots;
};

FileNames file_names(Venue venue, std::string_view upper_symbol, std::string_view date);

// Creates `dir` (and parents) and checks that a file can be created in it. Throws IoError naming
// the path. Returns the absolute path.
std::filesystem::path prepare_output_dir(const std::filesystem::path& dir);

// Moves every existing file among `names` into one new `replaced_<run_stamp>[-k]/` directory
// under `dir`, keeping the names. Called once per run with the names of all symbols; returns
// the archive directory, or nothing when none of the names existed. Throws IoError.
std::optional<std::filesystem::path> archive_existing(const std::filesystem::path& dir,
                                                      const std::vector<std::string>& names,
                                                      std::string_view run_stamp);

// The three output files of one symbol, created with their header rows (design §3.1, §6.6).
// The market-data and order-book files own 64 KiB buffers. The sidecar is built over the
// processor-wide scratch buffer shared by all symbols; its header and every row are flushed as
// soon as they end, so the scratch is always empty when another symbol's row starts.
class SymbolFiles {
public:
    static constexpr std::size_t kBufferBytes = std::size_t{64} * 1024;

    // Throws IoError if a file cannot be created.
    SymbolFiles(const std::filesystem::path& dir, const FileNames& names, char* sidecar_scratch,
                std::size_t scratch_capacity);
    SymbolFiles(const SymbolFiles&) = delete;
    ~SymbolFiles() = default;
    SymbolFiles& operator=(const SymbolFiles&) = delete;
    SymbolFiles(SymbolFiles&&) = delete;
    SymbolFiles& operator=(SymbolFiles&&) = delete;

    OutputFile& market_data() noexcept { return market_data_; }
    OutputFile& orderbook() noexcept { return orderbook_; }
    OutputFile& snapshots() noexcept { return snapshots_; }

    // Called after each sidecar row's terminator: flushes the shared scratch.
    bool end_snapshot_row() noexcept { return snapshots_.flush(); }

    bool failed() const noexcept;
    bool flush() noexcept;   // flushes all three; false on any failure
    bool finish() noexcept;  // flushes and closes all three; idempotent, failures sticky

private:
    std::unique_ptr<char[]> market_buffer_;  // NOLINT(modernize-avoid-c-arrays): dynamically sized
                                             // preallocated backing storage
    std::unique_ptr<char[]> book_buffer_;    // NOLINT(modernize-avoid-c-arrays): dynamically sized
                                             // preallocated backing storage
    OutputFile market_data_;
    OutputFile orderbook_;
    OutputFile snapshots_;
    bool finished_ = false;
    bool finish_ok_ = true;
};

}  // namespace blc
