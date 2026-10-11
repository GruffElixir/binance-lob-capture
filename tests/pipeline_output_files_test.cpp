#include "blc/core/errors.hpp"
#include "blc/csv/csv_writer.hpp"
#include "blc/csv/schema.hpp"
#include "blc/pipeline/output_files.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace blc {
namespace {

namespace fs = std::filesystem;
using test::read_file;
using test::TempDir;
using test::write_file;

constexpr std::size_t kScratch = 64 * 1024;

std::vector<std::string> all_names(const FileNames& n) {
    return {n.market_data, n.orderbook, n.snapshots};
}

std::string first_line(const fs::path& p) {
    const std::string s = read_file(p);
    return s.substr(0, s.find('\n') + 1);
}

TEST(OutputFiles, Names) {
    const FileNames n = file_names(Venue::Spot, "BTCUSDT", "2026-10-11");
    EXPECT_EQ(n.market_data, "market_data_spot_BTCUSDT_2026-10-11.csv");
    EXPECT_EQ(n.orderbook, "market_data_spot_BTCUSDT_2026-10-11_orderbook.csv");
    EXPECT_EQ(n.snapshots, "depth_snapshots_spot_BTCUSDT_2026-10-11.csv");
    const FileNames u = file_names(Venue::Usdm, "ETHUSDT", "2026-01-02");
    EXPECT_EQ(u.market_data, "market_data_usdm_ETHUSDT_2026-01-02.csv");
}

TEST(OutputFiles, ArchiveWholeSetKeepsNames) {
    TempDir dir;
    const auto names = all_names(file_names(Venue::Spot, "BTCUSDT", "2026-10-11"));
    for (const auto& n : names) {
        write_file(dir / n, "old " + n);
    }
    const auto archived = archive_existing(dir.path(), names, "20261011T120000Z");
    ASSERT_TRUE(archived.has_value());
    EXPECT_EQ(*archived, dir / "replaced_20261011T120000Z");
    for (const auto& n : names) {
        EXPECT_FALSE(fs::exists(dir / n)) << n;
        EXPECT_EQ(read_file(*archived / n), "old " + n);
    }
}

TEST(OutputFiles, ArchivePartialSetAndCollision) {
    TempDir dir;
    const FileNames n = file_names(Venue::Usdm, "BTCUSDT", "2026-10-11");
    const auto names = all_names(n);
    EXPECT_FALSE(archive_existing(dir.path(), names, "20261011T120000Z").has_value());
    EXPECT_FALSE(fs::exists(dir / "replaced_20261011T120000Z"));

    write_file(dir / n.orderbook, "ob");
    const auto first = archive_existing(dir.path(), names, "20261011T120000Z");
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(read_file(*first / n.orderbook), "ob");
    EXPECT_FALSE(fs::exists(*first / n.market_data));

    write_file(dir / n.market_data, "md");
    const auto second = archive_existing(dir.path(), names, "20261011T120000Z");
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, dir / "replaced_20261011T120000Z-1");
    EXPECT_EQ(read_file(*second / n.market_data), "md");

    write_file(dir / n.snapshots, "ss");
    const auto third = archive_existing(dir.path(), names, "20261011T120000Z");
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(*third, dir / "replaced_20261011T120000Z-2");
}

TEST(OutputFiles, ArchiveOncePerRunForAllSymbols) {
    TempDir dir;
    auto names = all_names(file_names(Venue::Spot, "BTCUSDT", "2026-10-11"));
    const auto eth = all_names(file_names(Venue::Spot, "ETHUSDT", "2026-10-11"));
    names.insert(names.end(), eth.begin(), eth.end());
    for (const auto& n : names) {
        write_file(dir / n, n);
    }
    const auto archived = archive_existing(dir.path(), names, "20261011T120000Z");
    ASSERT_TRUE(archived.has_value());
    std::size_t archive_dirs = 0;
    for (const auto& e : fs::directory_iterator(dir.path())) {
        if (e.path().filename().string().rfind("replaced_", 0) == 0) {
            ++archive_dirs;
        }
    }
    EXPECT_EQ(archive_dirs, 1u);
    for (const auto& n : names) {
        EXPECT_EQ(read_file(*archived / n), n);
    }
}

TEST(OutputFiles, OutputDirIsAFile) {
    TempDir dir;
    const fs::path file = dir / "not_a_dir";
    write_file(file, "x");
    try {
        prepare_output_dir(file);
        FAIL() << "expected IoError";
    } catch (const IoError& e) {
        EXPECT_NE(std::string(e.what()).find(file.string()), std::string::npos) << e.what();
    }
}

TEST(OutputFiles, OutputDirCreated) {
    TempDir dir;
    const fs::path nested = dir / "a" / "b";
    const fs::path got = prepare_output_dir(nested);
    EXPECT_TRUE(fs::is_directory(nested));
    EXPECT_TRUE(fs::equivalent(got, nested));
    EXPECT_EQ(std::distance(fs::directory_iterator(nested), fs::directory_iterator{}), 0);
}

TEST(OutputFiles, OutputDirNotWritable) {
#ifdef _WIN32
    GTEST_SKIP() << "POSIX permissions only";
#else
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root ignores directory permissions";
    }
    TempDir dir;
    const fs::path ro = dir / "ro";
    fs::create_directory(ro);
    ASSERT_EQ(::chmod(ro.c_str(), 0555), 0);
    EXPECT_THROW(prepare_output_dir(ro), IoError);
    ::chmod(ro.c_str(), 0755);
#endif
}

TEST(OutputFiles, HeadersWritten) {
    TempDir dir;
    auto scratch = std::make_unique<char[]>(kScratch);
    const FileNames n = file_names(Venue::Spot, "BTCUSDT", "2026-10-11");
    {
        SymbolFiles files(dir.path(), n, scratch.get(), kScratch);
        // The sidecar header is on disk at once; the other two are buffered until flushed.
        EXPECT_EQ(read_file(dir / n.snapshots), std::string(kSnapshotHeader) + "\n");
        EXPECT_TRUE(files.finish());
        EXPECT_TRUE(files.finish());  // idempotent
    }
    EXPECT_EQ(read_file(dir / n.market_data), std::string(kMarketDataHeader) + "\n");
    EXPECT_EQ(read_file(dir / n.orderbook), std::string(kOrderBookHeader) + "\n");
    EXPECT_EQ(read_file(dir / n.snapshots), std::string(kSnapshotHeader) + "\n");
}

void write_sidecar_row(SymbolFiles& files, const std::string& tag, std::size_t payload_bytes) {
    std::string payload = "{\"" + tag + "\":\"";
    while (payload.size() < payload_bytes) {
        payload += "\"\"x";  // quotes double when escaped
    }
    payload += "\"}";
    CsvRow(files.snapshots())
        .integer(1)
        .integer(2)
        .text("spot")
        .integer(0)
        .integer(0)
        .text(tag)
        .integer(3)
        .text(payload)
        .end();
    ASSERT_TRUE(files.end_snapshot_row());
}

TEST(OutputFiles, SidecarHeadersSurviveSharedScratch) {
    TempDir dir;
    auto scratch = std::make_unique<char[]>(kScratch);
    const FileNames a = file_names(Venue::Spot, "BTCUSDT", "2026-10-11");
    const FileNames b = file_names(Venue::Usdm, "ETHUSDT", "2026-10-11");
    SymbolFiles fa(dir.path(), a, scratch.get(), kScratch);
    SymbolFiles fb(dir.path(), b, scratch.get(), kScratch);
    write_sidecar_row(fb, "ETHUSDT", 70 * 1024);
    ASSERT_TRUE(fa.finish());
    ASSERT_TRUE(fb.finish());
    EXPECT_EQ(read_file(dir / a.snapshots), std::string(kSnapshotHeader) + "\n");
    EXPECT_EQ(first_line(dir / b.snapshots), std::string(kSnapshotHeader) + "\n");
    EXPECT_GT(read_file(dir / b.snapshots).size(), 70u * 1024);
}

TEST(OutputFiles, SharedSidecarScratchKeepsRowsSeparate) {
    TempDir dir;
    auto scratch = std::make_unique<char[]>(kScratch);
    const FileNames a = file_names(Venue::Spot, "BTCUSDT", "2026-10-11");
    const FileNames b = file_names(Venue::Spot, "ETHUSDT", "2026-10-11");
    SymbolFiles fa(dir.path(), a, scratch.get(), kScratch);
    SymbolFiles fb(dir.path(), b, scratch.get(), kScratch);
    write_sidecar_row(fa, "BTCUSDT", 40 * 1024);
    write_sidecar_row(fb, "ETHUSDT", 40 * 1024);
    const std::string sa = read_file(dir / a.snapshots);
    const std::string sb = read_file(dir / b.snapshots);
    EXPECT_NE(sa.find("BTCUSDT"), std::string::npos);
    EXPECT_EQ(sa.find("ETHUSDT"), std::string::npos);
    EXPECT_NE(sb.find("ETHUSDT"), std::string::npos);
    EXPECT_EQ(sb.find("BTCUSDT"), std::string::npos);
    // Each file is its header plus exactly one complete row.
    EXPECT_EQ(std::count(sa.begin(), sa.end(), '\n'), 2);
    EXPECT_EQ(std::count(sb.begin(), sb.end(), '\n'), 2);
    EXPECT_EQ(sa.back(), '\n');
    EXPECT_TRUE(fa.finish());
    EXPECT_TRUE(fb.finish());
}

}  // namespace
}  // namespace blc
