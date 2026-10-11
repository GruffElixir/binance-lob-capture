#include "blc/csv/csv_writer.hpp"

#include "blc/core/errors.hpp"

#include <array>
#include <charconv>
#include <cstring>
#include <utility>

namespace blc {

bool needs_quoting(std::string_view field) noexcept {
    return field.find_first_of(",\"\r\n") != std::string_view::npos;
}

OutputFile OutputFile::create(const std::filesystem::path& path, char* buffer,
                              std::size_t capacity) {
#if defined(_WIN32)
    std::FILE* f = _wfopen(path.c_str(), L"wb");
#else
    std::FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (f == nullptr) {
        throw IoError("cannot create output file: " + path.u8string());
    }
    if (std::setvbuf(f, nullptr, _IONBF, 0) != 0) {
        (void)std::fclose(f);
        throw IoError("cannot configure output buffering: " + path.u8string());
    }
    return {f, capacity == 0 ? nullptr : buffer, buffer == nullptr ? 0 : capacity};
}

OutputFile::OutputFile(OutputFile&& other) noexcept
    : file_(std::exchange(other.file_, nullptr)),
      buffer_(std::exchange(other.buffer_, nullptr)),
      capacity_(std::exchange(other.capacity_, 0)),
      size_(std::exchange(other.size_, 0)),
      failed_(std::exchange(other.failed_, false)) {}

OutputFile& OutputFile::operator=(OutputFile&& other) noexcept {
    if (this != &other) {
        close();
        file_ = std::exchange(other.file_, nullptr);
        buffer_ = std::exchange(other.buffer_, nullptr);
        capacity_ = std::exchange(other.capacity_, 0);
        size_ = std::exchange(other.size_, 0);
        failed_ = std::exchange(other.failed_, false);
    }
    return *this;
}

OutputFile::~OutputFile() {
    close();
}

bool OutputFile::write_raw(const char* data, std::size_t n) noexcept {
    if (failed_ || file_ == nullptr) {
        failed_ = true;
        return false;
    }
    if (n != 0 && std::fwrite(data, 1, n, file_) != n) {
        failed_ = true;
        return false;
    }
    return true;
}

void OutputFile::write(std::string_view bytes) noexcept {
    if (failed_) {
        return;
    }
    if (capacity_ == 0) {
        write_raw(bytes.data(), bytes.size());
        return;
    }
    while (!bytes.empty()) {
        if (size_ == capacity_ && !flush()) {
            return;
        }
        const std::size_t n = std::min(bytes.size(), capacity_ - size_);
        std::memcpy(buffer_ + size_, bytes.data(), n);
        size_ += n;
        bytes.remove_prefix(n);
    }
}

bool OutputFile::flush() noexcept {
    if (failed_) {
        return false;
    }
    if (size_ != 0) {
        const bool ok = write_raw(buffer_, size_);
        size_ = 0;
        if (!ok) {
            return false;
        }
    }
    if (file_ != nullptr && std::fflush(file_) != 0) {
        failed_ = true;
    }
    return !failed_;
}

bool OutputFile::close() noexcept {
    if (file_ == nullptr) {
        return !failed_;
    }
    bool ok = flush();
    if (std::fclose(file_) != 0) {
        ok = false;
        failed_ = true;
    }
    file_ = nullptr;
    return ok;
}

void CsvRow::separator() noexcept {
    if (!first_) {
        file_.write_char(',');
    }
    first_ = false;
}

CsvRow& CsvRow::integer(std::int64_t value) noexcept {
    separator();
    std::array<char, 24> buf{};
    const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    file_.write(std::string_view(buf.data(), static_cast<std::size_t>(res.ptr - buf.data())));
    return *this;
}

CsvRow& CsvRow::unsigned_integer(std::uint64_t value) noexcept {
    separator();
    std::array<char, 24> buf{};
    const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    file_.write(std::string_view(buf.data(), static_cast<std::size_t>(res.ptr - buf.data())));
    return *this;
}

CsvRow& CsvRow::character(char c) noexcept {
    return text(std::string_view(&c, 1));
}

CsvRow& CsvRow::text(std::string_view field) noexcept {
    separator();
    if (!needs_quoting(field)) {
        file_.write(field);
        return *this;
    }
    file_.write_char('"');
    std::size_t start = 0;
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '"') {
            file_.write(field.substr(start, i + 1 - start));  // includes this quote
            file_.write_char('"');                            // doubled
            start = i + 1;
        }
    }
    file_.write(field.substr(start));
    file_.write_char('"');
    return *this;
}

void CsvRow::end() noexcept {
    file_.write_char('\n');
}

}  // namespace blc
