#pragma once

#include "blc/core/errors.hpp"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace blc {

struct CsvFormatError : IoError {
    CsvFormatError(const std::string& what, std::size_t line_number)
        : IoError(what), line(line_number) {}
    std::size_t line;
};

// Streaming RFC 4180 reader. Memory is bounded by a 1 MiB chunk plus the longest row seen,
// independent of file size. Accepts LF and CRLF, a missing final newline and trailing blank
// lines. Field views returned by next() stay valid until the following call.
class CsvReader {
public:
    static CsvReader open(const std::filesystem::path& path);  // throws IoError

    CsvReader(CsvReader&& other) noexcept;
    CsvReader& operator=(CsvReader&&) = delete;
    CsvReader(const CsvReader&) = delete;
    CsvReader& operator=(const CsvReader&) = delete;
    ~CsvReader();

    // Reads the next non-blank row; false at end of file. Throws CsvFormatError.
    bool next(std::vector<std::string_view>& fields);

    // 1-based line on which the most recent row (or the row in error) starts.
    std::size_t line() const noexcept { return row_line_; }

private:
    static constexpr std::size_t kChunkBytes = std::size_t{1} << 20;

    explicit CsvReader(std::FILE* file);
    int get();  // next byte or EOF
    int peek();

    std::FILE* file_ = nullptr;
    std::vector<char> chunk_;
    std::size_t chunk_pos_ = 0;
    std::size_t chunk_len_ = 0;
    bool eof_ = false;
    std::string row_;
    std::vector<std::size_t> field_ends_;
    std::size_t current_line_ = 1;
    std::size_t row_line_ = 0;
};

}  // namespace blc
