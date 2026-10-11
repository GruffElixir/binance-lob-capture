#include "blc/pipeline/output_files.hpp"

#include "blc/core/errors.hpp"
#include "blc/csv/schema.hpp"

#include <fstream>
#include <system_error>

namespace blc {

namespace fs = std::filesystem;

namespace {

[[noreturn]] void fail(const fs::path& path, const std::string& what) {
    throw IoError(what + ": " + path.string());
}

OutputFile create_with_header(const fs::path& path, char* buffer, std::size_t capacity,
                              std::string_view header) {
    OutputFile file = OutputFile::create(path, buffer, capacity);
    file.write(header);
    file.write_char('\n');
    return file;
}

}  // namespace

FileNames file_names(Venue venue, std::string_view upper_symbol, std::string_view date) {
    std::string stem;
    stem.append(to_string(venue)).append("_").append(upper_symbol).append("_").append(date);
    FileNames n;
    n.market_data = "market_data_" + stem + ".csv";
    n.orderbook = "market_data_" + stem + "_orderbook.csv";
    n.snapshots = "depth_snapshots_" + stem + ".csv";
    return n;
}

fs::path prepare_output_dir(const fs::path& dir) {
    std::error_code ec;
    const fs::file_status st = fs::status(dir, ec);
    if (fs::exists(st)) {
        if (!fs::is_directory(st)) {
            fail(dir, "output directory is not a directory");
        }
    } else if (!fs::create_directories(dir, ec) || ec) {
        fail(dir, "cannot create output directory (" + ec.message() + ")");
    }
    // Permission bits alone do not tell (ACLs, read-only mounts): try to create a file.
    const fs::path probe = dir / ".binance_capture_write_probe";
    {
        std::ofstream const out(probe, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail(dir, "output directory is not writable");
        }
    }
    fs::remove(probe, ec);
    const fs::path absolute = fs::absolute(dir, ec);
    return ec ? dir : absolute;
}

std::optional<fs::path> archive_existing(const fs::path& dir, const std::vector<std::string>& names,
                                         std::string_view run_stamp) {
    std::vector<const std::string*> existing;
    for (const auto& name : names) {
        std::error_code ec;
        if (fs::exists(fs::symlink_status(dir / name, ec))) {
            existing.push_back(&name);
        }
    }
    if (existing.empty()) {
        return std::nullopt;
    }
    const std::string base = "replaced_" + std::string(run_stamp);
    fs::path archive = dir / base;
    for (unsigned k = 1;; ++k) {
        std::error_code ec;
        if (fs::create_directory(archive, ec)) {
            break;
        }
        if (ec || k > 10000) {
            fail(archive, "cannot create archive directory (" + ec.message() + ")");
        }
        archive = dir / (base + "-" + std::to_string(k));
    }
    for (const std::string* name : existing) {
        std::error_code ec;
        fs::rename(dir / *name, archive / *name, ec);
        if (ec) {
            fail(dir / *name,
                 "cannot move existing file to " + archive.string() + " (" + ec.message() + ")");
        }
    }
    return archive;
}

SymbolFiles::SymbolFiles(const fs::path& dir, const FileNames& names, char* sidecar_scratch,
                         std::size_t scratch_capacity)
    // NOLINTNEXTLINE(modernize-avoid-c-arrays): preallocated writer buffer
    : market_buffer_(std::make_unique<char[]>(kBufferBytes)),
      // NOLINTNEXTLINE(modernize-avoid-c-arrays): preallocated writer buffer
      book_buffer_(std::make_unique<char[]>(kBufferBytes)),
      market_data_(create_with_header(dir / names.market_data, market_buffer_.get(), kBufferBytes,
                                      kMarketDataHeader)),
      orderbook_(create_with_header(dir / names.orderbook, book_buffer_.get(), kBufferBytes,
                                    kOrderBookHeader)),
      snapshots_(create_with_header(dir / names.snapshots, sidecar_scratch, scratch_capacity,
                                    kSnapshotHeader)) {
    if (!snapshots_.flush()) {
        fail(dir / names.snapshots, "cannot write");
    }
}

bool SymbolFiles::failed() const noexcept {
    return market_data_.failed() || orderbook_.failed() || snapshots_.failed();
}

bool SymbolFiles::flush() noexcept {
    if (finished_) {
        return finish_ok_;
    }
    const bool a = market_data_.flush();
    const bool b = orderbook_.flush();
    const bool c = snapshots_.flush();
    return a && b && c;
}

bool SymbolFiles::finish() noexcept {
    if (!finished_) {
        finished_ = true;
        const bool a = market_data_.close();
        const bool b = orderbook_.close();
        const bool c = snapshots_.close();
        finish_ok_ = a && b && c;
    }
    return finish_ok_;
}

}  // namespace blc
