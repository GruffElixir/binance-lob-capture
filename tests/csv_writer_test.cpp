#include "blc/csv/csv_writer.hpp"

#include "blc/core/errors.hpp"
#include "support/temp_dir.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

#include <gtest/gtest.h>

namespace blc {
namespace {

using test::read_file;
using test::TempDir;

TEST(CsvWriter, QuotesOnlyWhenNeeded) {
    TempDir dir;
    std::array<char, 16> buffer{};  // tiny buffer forces flushes in the middle of a row
    {
        auto file = OutputFile::create(dir / "a.csv", buffer.data(), buffer.size());
        CsvRow(file)
            .integer(1791669451)
            .text("spot")
            .text(R"({"e":"trade","p":"1,5"})")
            .character('B')
            .end();
        EXPECT_TRUE(file.close());
    }
    EXPECT_EQ(read_file(dir / "a.csv"),
              "1791669451,spot,\"{\"\"e\"\":\"\"trade\"\",\"\"p\"\":\"\"1,5\"\"}\",B\n");
}

TEST(CsvWriter, EmbeddedNewlineAndCr) {
    TempDir dir;
    std::array<char, 64> buffer{};
    {
        auto file = OutputFile::create(dir / "b.csv", buffer.data(), buffer.size());
        CsvRow(file).text("a\nb").text("a\rb").text("plain").end();
        EXPECT_TRUE(file.close());
    }
    EXPECT_EQ(read_file(dir / "b.csv"), "\"a\nb\",\"a\rb\",plain\n");
}

TEST(CsvWriter, NegativeAndZero) {
    TempDir dir;
    std::array<char, 64> buffer{};
    {
        auto file = OutputFile::create(dir / "c.csv", buffer.data(), buffer.size());
        CsvRow(file).integer(0).integer(std::numeric_limits<std::int64_t>::min()).end();
        EXPECT_TRUE(file.close());
    }
    EXPECT_EQ(read_file(dir / "c.csv"), "0,-9223372036854775808\n");
}

TEST(CsvWriter, UnsignedBoundary) {
    TempDir dir;
    std::array<char, 64> buffer{};
    {
        auto file = OutputFile::create(dir / "d.csv", buffer.data(), buffer.size());
        CsvRow(file).unsigned_integer(std::numeric_limits<std::uint64_t>::max()).end();
        EXPECT_TRUE(file.close());
    }
    EXPECT_EQ(read_file(dir / "d.csv"), "18446744073709551615\n");
}

TEST(CsvWriter, ZeroCapacityWritesThrough) {
    TempDir dir;
    {
        auto file = OutputFile::create(dir / "e.csv", nullptr, 0);
        CsvRow(file).text("x,y").integer(7).end();
        EXPECT_TRUE(file.close());
    }
    EXPECT_EQ(read_file(dir / "e.csv"), "\"x,y\",7\n");
}

TEST(CsvWriter, BinaryModeKeepsLf) {
    TempDir dir;
    std::array<char, 8> buffer{};
    {
        auto file = OutputFile::create(dir / "f.csv", buffer.data(), buffer.size());
        file.write("a\nb\n");
        EXPECT_TRUE(file.close());
    }
    EXPECT_EQ(read_file(dir / "f.csv"), "a\nb\n");  // no CRLF translation on Windows
}

TEST(CsvWriter, CreateFailureThrowsIoError) {
    TempDir dir;
    std::array<char, 8> buffer{};
    EXPECT_THROW(OutputFile::create(dir / "missing_dir" / "x.csv", buffer.data(), buffer.size()),
                 IoError);
}

TEST(CsvWriter, NeedsQuoting) {
    EXPECT_FALSE(needs_quoting("abc"));
    EXPECT_TRUE(needs_quoting("a,b"));
    EXPECT_TRUE(needs_quoting("a\"b"));
    EXPECT_TRUE(needs_quoting("a\nb"));
    EXPECT_TRUE(needs_quoting("a\rb"));
}

TEST(CsvWriter, FailedWriteIsSticky) {
#if defined(__linux__)
    std::array<char, 16> buffer{};
    auto file = OutputFile::create("/dev/full", buffer.data(), buffer.size());
    file.write("0123456789");
    EXPECT_FALSE(file.flush());
    EXPECT_TRUE(file.failed());
    file.write("more");
    EXPECT_TRUE(file.failed());
    EXPECT_FALSE(file.close());
#else
    GTEST_SKIP() << "/dev/full is Linux-only";
#endif
}

}  // namespace
}  // namespace blc
