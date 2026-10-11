#include "blc/csv/csv_reader.hpp"

#include <utility>

namespace blc {

CsvReader CsvReader::open(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::FILE* f = _wfopen(path.c_str(), L"rb");
#else
    std::FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (f == nullptr) {
        throw IoError("cannot open input file: " + path.u8string());
    }
    return CsvReader(f);
}

CsvReader::CsvReader(std::FILE* file) : file_(file), chunk_(kChunkBytes) {}

CsvReader::CsvReader(CsvReader&& other) noexcept
    : file_(std::exchange(other.file_, nullptr)),
      chunk_(std::move(other.chunk_)),
      chunk_pos_(other.chunk_pos_),
      chunk_len_(other.chunk_len_),
      eof_(other.eof_),
      row_(std::move(other.row_)),
      field_ends_(std::move(other.field_ends_)),
      current_line_(other.current_line_),
      row_line_(other.row_line_) {}

CsvReader::~CsvReader() {
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

int CsvReader::peek() {
    if (chunk_pos_ == chunk_len_) {
        if (eof_) {
            return EOF;
        }
        chunk_len_ = std::fread(chunk_.data(), 1, chunk_.size(), file_);
        chunk_pos_ = 0;
        if (chunk_len_ == 0) {
            eof_ = true;
            if (std::ferror(file_) != 0) {
                throw IoError("read error");
            }
            return EOF;
        }
    }
    return static_cast<unsigned char>(chunk_[chunk_pos_]);
}

int CsvReader::get() {
    const int c = peek();
    if (c != EOF) {
        ++chunk_pos_;
        if (c == '\n') {
            ++current_line_;
        }
    }
    return c;
}

bool CsvReader::next(std::vector<std::string_view>& fields) {
    enum class State { FieldStart, Unquoted, Quoted, QuoteInQuoted };

    for (;;) {
        row_.clear();
        field_ends_.clear();
        row_line_ = current_line_;
        State state = State::FieldStart;
        std::size_t unquoted_start = 0;
        bool row_has_content = false;
        bool row_done = false;

        // Ends an unquoted field at a line break, dropping the CR of a CRLF terminator.
        const auto end_unquoted_at_eol = [&] {
            if (row_.size() > unquoted_start && row_.back() == '\r') {
                row_.pop_back();
            }
            field_ends_.push_back(row_.size());
        };

        while (!row_done) {
            const int c = get();
            switch (state) {
                case State::FieldStart:
                    if (c == EOF || c == '\n') {
                        if (row_has_content) {
                            field_ends_.push_back(row_.size());
                        }
                        row_done = true;
                    } else if (c == '"') {
                        state = State::Quoted;
                        row_has_content = true;
                    } else if (c == ',') {
                        field_ends_.push_back(row_.size());
                        row_has_content = true;
                    } else {
                        unquoted_start = row_.size();
                        row_.push_back(static_cast<char>(c));
                        state = State::Unquoted;
                        if (c != '\r') {
                            row_has_content = true;
                        }
                    }
                    break;
                case State::Unquoted:
                    if (c == EOF || c == '\n') {
                        end_unquoted_at_eol();
                        row_done = true;
                    } else if (c == ',') {
                        field_ends_.push_back(row_.size());
                        state = State::FieldStart;
                        row_has_content = true;
                    } else if (c == '"') {
                        throw CsvFormatError("stray quote in unquoted field", row_line_);
                    } else {
                        row_.push_back(static_cast<char>(c));
                        if (c != '\r') {
                            row_has_content = true;
                        }
                    }
                    break;
                case State::Quoted:
                    if (c == EOF) {
                        throw CsvFormatError("unterminated quoted field", row_line_);
                    }
                    if (c == '"') {
                        state = State::QuoteInQuoted;
                    } else {
                        row_.push_back(static_cast<char>(c));
                    }
                    break;
                case State::QuoteInQuoted:
                    if (c == '"') {
                        row_.push_back('"');
                        state = State::Quoted;
                    } else if (c == ',') {
                        field_ends_.push_back(row_.size());
                        state = State::FieldStart;
                    } else if (c == EOF || c == '\n') {
                        field_ends_.push_back(row_.size());
                        row_done = true;
                    } else if (c == '\r' && peek() == '\n') {
                        get();
                        field_ends_.push_back(row_.size());
                        row_done = true;
                    } else {
                        throw CsvFormatError("unexpected character after closing quote", row_line_);
                    }
                    break;
            }
            if (row_done && c == EOF && !row_has_content) {
                return false;
            }
        }

        if (!row_has_content) {
            continue;  // blank line (LF or CRLF)
        }
        // The row's storage is final now, so views into it stay valid until the next call.
        fields.clear();
        std::size_t begin = 0;
        for (const std::size_t end : field_ends_) {
            fields.emplace_back(row_.data() + begin, end - begin);
            begin = end;
        }
        return true;
    }
}

}  // namespace blc
