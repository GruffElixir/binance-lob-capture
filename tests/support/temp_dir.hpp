#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

namespace blc::test {

// A unique, empty directory for one test; removed (recursively) on destruction.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        std::string name = info != nullptr
                               ? std::string(info->test_suite_name()) + "_" + info->name()
                               : std::string("blc");
        for (char& c : name) {
            if (c == '/' || c == '\\') {
                c = '_';
            }
        }
        path_ = std::filesystem::temp_directory_path() /
                ("blc_" + name + "_" + std::to_string(counter.fetch_add(1)) + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this) & 0xFFFFFF));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

inline std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void write_file(const std::filesystem::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace blc::test
