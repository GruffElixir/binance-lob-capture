#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string_view>

namespace blc {

// RFC 4180 rule: quote a field if and only if it contains a comma, quote, CR or LF.
bool needs_quoting(std::string_view field) noexcept;

// An output file with a caller-owned user-space buffer over an unbuffered std::FILE.
// Writes never throw; the first failure is sticky and reported by failed()/flush()/close().
class OutputFile {
public:
    // Opens `path` for writing in binary mode (truncating). Throws IoError on failure.
    // `buffer` may be null with `capacity` 0, in which case every write goes straight through.
    static OutputFile create(const std::filesystem::path& path, char* buffer, std::size_t capacity);

    OutputFile(OutputFile&& other) noexcept;
    OutputFile& operator=(OutputFile&& other) noexcept;
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    ~OutputFile();

    void write(std::string_view bytes) noexcept;
    void write_char(char c) noexcept { write(std::string_view(&c, 1)); }
    bool flush() noexcept;  // false if this or any earlier write failed
    bool close() noexcept;  // flushes, closes; false on any failure
    bool failed() const noexcept { return failed_; }
    bool is_open() const noexcept { return file_ != nullptr; }
    std::size_t buffered() const noexcept { return size_; }

private:
    OutputFile(std::FILE* file, char* buffer, std::size_t capacity) noexcept
        : file_(file), buffer_(buffer), capacity_(capacity) {}
    bool write_raw(const char* data, std::size_t n) noexcept;

    std::FILE* file_ = nullptr;
    char* buffer_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
    bool failed_ = false;
};

// Writes one CSV record field by field; separators and quoting are handled here.
class CsvRow {
public:
    explicit CsvRow(OutputFile& file) noexcept : file_(file) {}

    CsvRow& integer(std::int64_t value) noexcept;
    CsvRow& unsigned_integer(std::uint64_t value) noexcept;
    CsvRow& character(char c) noexcept;
    CsvRow& text(std::string_view field) noexcept;  // quoted only when needed
    void end() noexcept;                            // writes the record terminator '\n'

private:
    void separator() noexcept;

    OutputFile& file_;
    bool first_ = true;
};

}  // namespace blc
