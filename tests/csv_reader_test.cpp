#include "blc/csv/csv_reader.hpp"

#include "blc/core/errors.hpp"
#include "blc/csv/csv_writer.hpp"
#include "support/temp_dir.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

using test::TempDir;
using test::write_file;

std::vector<std::vector<std::string>> read_all(const std::filesystem::path& p) {
    auto reader = CsvReader::open(p);
    std::vector<std::string_view> fields;
    std::vector<std::vector<std::string>> rows;
    while (reader.next(fields)) {
        rows.emplace_back(fields.begin(), fields.end());
    }
    return rows;
}

using Rows = std::vector<std::vector<std::string>>;

TEST(CsvReader, RoundTripsWriterOutput) {
    TempDir dir;
    std::array<char, 16> buffer{};
    {
        auto f = OutputFile::create(dir / "w.csv", buffer.data(), buffer.size());
        CsvRow(f)
            .integer(1791669451)
            .text("spot")
            .text(R"({"e":"trade","p":"1,5"})")
            .character('B')
            .end();
        CsvRow(f).text("a\nb").text("a\rb").text("").end();
        CsvRow(f).integer(std::numeric_limits<std::int64_t>::min()).unsigned_integer(7).end();
        ASSERT_TRUE(f.close());
    }
    EXPECT_EQ(read_all(dir / "w.csv"),
              (Rows{{"1791669451", "spot", R"({"e":"trade","p":"1,5"})", "B"},
                    {"a\nb", "a\rb", ""},
                    {"-9223372036854775808", "7"}}));
}

TEST(CsvReader, CrlfAndNoFinalNewline) {
    TempDir dir;
    write_file(dir / "c.csv", "a,b\r\n\"x,\"\"y\"\"\",2");
    EXPECT_EQ(read_all(dir / "c.csv"), (Rows{{"a", "b"}, {"x,\"y\"", "2"}}));
}

TEST(CsvReader, TrailingBlankLinesIgnored) {
    TempDir dir;
    write_file(dir / "t.csv", "a\n\n\n");
    EXPECT_EQ(read_all(dir / "t.csv"), (Rows{{"a"}}));
    write_file(dir / "t2.csv", "a\r\n\r\n");
    EXPECT_EQ(read_all(dir / "t2.csv"), (Rows{{"a"}}));
}

TEST(CsvReader, EmbeddedNewlineInQuotes) {
    TempDir dir;
    write_file(dir / "e.csv", "\"l1\nl2\",z\n");
    EXPECT_EQ(read_all(dir / "e.csv"), (Rows{{"l1\nl2", "z"}}));
}

TEST(CsvReader, EmptyFieldsKept) {
    TempDir dir;
    write_file(dir / "x.csv", ",a,\n");
    EXPECT_EQ(read_all(dir / "x.csv"), (Rows{{"", "a", ""}}));
}

TEST(CsvReader, FieldViewsSurviveRowGrowth) {
    TempDir dir;
    const std::string big(3u << 20, 'q');
    write_file(dir / "g.csv", "1,2,\"" + big + "\"\n3,4,5\n");
    auto reader = CsvReader::open(dir / "g.csv");
    std::vector<std::string_view> fields;
    ASSERT_TRUE(reader.next(fields));
    ASSERT_EQ(fields.size(), 3u);
    EXPECT_EQ(fields[0], "1");
    EXPECT_EQ(fields[1], "2");
    EXPECT_EQ(fields[2].size(), big.size());
    EXPECT_EQ(fields[2], big);
    ASSERT_TRUE(reader.next(fields));
    EXPECT_EQ(fields, (std::vector<std::string_view>{"3", "4", "5"}));
    EXPECT_FALSE(reader.next(fields));
}

TEST(CsvReader, RowSpanningChunkBoundary) {
    TempDir dir;
    std::string content;
    std::vector<std::vector<std::string>> expected;
    const std::string big(3u << 20, 'z');
    content += "\"" + big + "\",end\n";
    expected.push_back({big, "end"});
    // Many short rows so several straddle each 1 MiB boundary.
    for (int i = 0; i < 300'000; ++i) {
        const std::string a = std::to_string(i);
        content += a + ",\"q\"\"" + a + "\"\n";
        expected.push_back({a, "q\"" + a});
    }
    write_file(dir / "s.csv", content);
    EXPECT_EQ(read_all(dir / "s.csv"), expected);
}

TEST(CsvReader, UnterminatedQuoteThrowsWithLine) {
    TempDir dir;
    write_file(dir / "u.csv", "a\n\"b\n");
    auto reader = CsvReader::open(dir / "u.csv");
    std::vector<std::string_view> fields;
    ASSERT_TRUE(reader.next(fields));
    try {
        reader.next(fields);
        FAIL() << "expected CsvFormatError";
    } catch (const CsvFormatError& e) {
        EXPECT_EQ(e.line, 2u);
        EXPECT_EQ(reader.line(), 2u);
    }
}

TEST(CsvReader, StrayQuoteInUnquotedFieldThrows) {
    TempDir dir;
    write_file(dir / "s.csv", "ab\"c,d\n");
    auto reader = CsvReader::open(dir / "s.csv");
    std::vector<std::string_view> fields;
    EXPECT_THROW(reader.next(fields), CsvFormatError);
}

TEST(CsvReader, OpenMissingFileThrowsIoError) {
    TempDir dir;
    EXPECT_THROW(CsvReader::open(dir / "nope.csv"), IoError);
}

}  // namespace
}  // namespace blc
