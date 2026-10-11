#include "blc/replay/replay.hpp"

#include "blc/cli/options.hpp"
#include "blc/core/types.hpp"
#include "blc/csv/schema.hpp"
#include "blc/pipeline/output_files.hpp"
#include "blc/verify/verify.hpp"
#include "support/loss_scenario.hpp"
#include "support/temp_dir.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

namespace fs = std::filesystem;
using test::read_file;
using test::TempDir;
using test::write_file;

const FileNames kBtc = file_names(Venue::Spot, "BTCUSDT", "2026-10-11");

ReplayOptions options(std::vector<fs::path> inputs, const fs::path& out) {
    ReplayOptions o;
    o.inputs = std::move(inputs);
    o.output_dir = out;
    return o;
}

int replay_capturing(const ReplayOptions& o, std::string& err) {
    ::testing::internal::CaptureStderr();
    const int code = run_replay(o);
    err = ::testing::internal::GetCapturedStderr();
    return code;
}

std::string to_crlf_without_final_newline(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '\n') {
            out += "\r\n";
        } else {
            out += c;
        }
    }
    out.resize(out.size() - 2);
    return out;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < s.size()) {
        const std::size_t nl = s.find('\n', start);
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) {
        out += l + "\n";
    }
    return out;
}

struct CaptureRun {
    CaptureRun() {
        test::write_loss_scenario(in.path());
        fs::create_directories(out.path());
    }
    fs::path md() const { return in / kBtc.market_data; }
    fs::path ob() const { return in / kBtc.orderbook; }
    fs::path sidecar() const { return in / kBtc.snapshots; }
    fs::path replayed() const { return out / kBtc.orderbook; }
    TempDir in;
    TempDir out;
};

TEST(Replay, GlobSkipsOrderbookFiles) {
    CaptureRun r;
    std::string err;
    EXPECT_EQ(replay_capturing(options({r.md(), r.ob()}, r.out.path()), err), 0) << err;
    EXPECT_NE(err.find("skipping"), std::string::npos) << err;
    EXPECT_EQ(read_file(r.replayed()), read_file(r.ob()));
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(r.out.path())) {
        (void)e;
        ++files;
    }
    EXPECT_EQ(files, 1u);
}

TEST(Replay, OnlyOrderbookFilesIsUsageError) {
    CaptureRun r;
    std::string err;
    EXPECT_EQ(replay_capturing(options({r.ob()}, r.out.path()), err), 2);
}

TEST(Replay, LossIntegrationReplaysIdentically) {
    CaptureRun r;
    std::string err;
    ASSERT_EQ(replay_capturing(options({r.md()}, r.out.path()), err), 0) << err;
    EXPECT_EQ(read_file(r.replayed()), read_file(r.ob()));
    EXPECT_NE(err.find("summary"), std::string::npos) << err;
}

TEST(Replay, FailingFileDoesNotStopOthers) {
    CaptureRun r;
    const fs::path bad = r.in / "market_data_spot_ETHUSDT_2026-10-11.csv";
    write_file(bad, "not,the,header\n");
    write_file(r.in / "depth_snapshots_spot_ETHUSDT_2026-10-11.csv",
               std::string(kSnapshotHeader) + "\n");
    std::string err;
    EXPECT_EQ(replay_capturing(options({bad, r.md()}, r.out.path()), err), 1);
    EXPECT_NE(err.find(bad.filename().string()), std::string::npos) << err;
    EXPECT_EQ(read_file(r.replayed()), read_file(r.ob()));
}

TEST(Replay, MissingInputDoesNotStopOthers) {
    CaptureRun r;
    std::string err;
    const fs::path missing = r.in / "market_data_spot_XRPUSDT_2026-10-11.csv";
    EXPECT_EQ(replay_capturing(options({missing, r.md()}, r.out.path()), err), 1);
    EXPECT_NE(err.find("XRPUSDT"), std::string::npos) << err;
    EXPECT_EQ(read_file(r.replayed()), read_file(r.ob()));
}

TEST(Replay, NothingReplayableNamesTheFile) {
    TempDir out;
    std::string err;
    EXPECT_EQ(replay_capturing(options({"nothing.csv"}, out.path()), err), 1);
    EXPECT_NE(err.find("nothing.csv"), std::string::npos) << err;
}

TEST(Replay, MissingSidecarFails) {
    CaptureRun r;
    fs::remove(r.sidecar());
    std::string err;
    EXPECT_EQ(replay_capturing(options({r.md()}, r.out.path()), err), 1);
    EXPECT_NE(err.find(kBtc.snapshots), std::string::npos) << err;
}

TEST(Replay, NonMonotonicApplyBeforeRowRejected) {
    CaptureRun r;
    auto lines = split_lines(read_file(r.sidecar()));
    ASSERT_EQ(lines.size(), 3u);  // header + one snapshot per epoch
    std::swap(lines[1], lines[2]);
    write_file(r.sidecar(), join_lines(lines));
    std::string err;
    EXPECT_EQ(replay_capturing(options({r.md()}, r.out.path()), err), 1);
    EXPECT_NE(err.find("apply_before_row"), std::string::npos) << err;
    EXPECT_FALSE(fs::exists(r.replayed()));  // no partial output is left behind
}

TEST(Replay, ApplyBeforeRowBeyondEndRejected) {
    CaptureRun r;
    auto lines = split_lines(read_file(r.sidecar()));
    const std::size_t comma = lines[2].find(",\"");
    const std::size_t prev = lines[2].rfind(',', comma - 1);
    lines[2] = lines[2].substr(0, prev + 1) + "999999" + lines[2].substr(comma);
    write_file(r.sidecar(), join_lines(lines));
    std::string err;
    EXPECT_EQ(replay_capturing(options({r.md()}, r.out.path()), err), 1);
    EXPECT_NE(err.find("apply_before_row"), std::string::npos) << err;
}

TEST(Replay, CrlfInputSameOutput) {
    CaptureRun r;
    write_file(r.md(), to_crlf_without_final_newline(read_file(r.md())));
    write_file(r.sidecar(), to_crlf_without_final_newline(read_file(r.sidecar())));
    std::string err;
    ASSERT_EQ(replay_capturing(options({r.md()}, r.out.path()), err), 0) << err;
    EXPECT_EQ(read_file(r.replayed()), read_file(r.ob()));
}

TEST(Replay, ExistingOutputArchived) {
    CaptureRun r;
    std::string err;
    ASSERT_EQ(replay_capturing(options({r.md()}, r.out.path()), err), 0) << err;
    ASSERT_EQ(replay_capturing(options({r.md()}, r.out.path()), err), 0) << err;
    std::vector<fs::path> archives;
    for (const auto& e : fs::directory_iterator(r.out.path())) {
        if (e.path().filename().string().rfind("replaced_", 0) == 0) {
            archives.push_back(e.path());
        }
    }
    ASSERT_EQ(archives.size(), 1u);
    EXPECT_EQ(read_file(archives[0] / kBtc.orderbook), read_file(r.ob()));
    EXPECT_EQ(read_file(r.replayed()), read_file(r.ob()));
}

TEST(Replay, SkipsByOrderbookHeader) {
    CaptureRun r;
    const fs::path odd = r.in / "market_data_x.csv";
    write_file(odd, std::string(kOrderBookHeader) + "\n");
    std::string err;
    EXPECT_EQ(replay_capturing(options({odd, r.md()}, r.out.path()), err), 0) << err;
    EXPECT_NE(err.find("market_data_x.csv"), std::string::npos) << err;
    EXPECT_EQ(replay_capturing(options({odd}, r.out.path()), err), 2);
}

TEST(Replay, HeaderOnlyInput) {
    TempDir in;
    TempDir out;
    write_file(in / kBtc.market_data, std::string(kMarketDataHeader) + "\n");
    write_file(in / kBtc.snapshots, std::string(kSnapshotHeader) + "\n");
    std::string err;
    EXPECT_EQ(replay_capturing(options({in / kBtc.market_data}, out.path()), err), 0) << err;
    EXPECT_EQ(read_file(out / kBtc.orderbook), std::string(kOrderBookHeader) + "\n");
}

TEST(Replay, ArchivedRunReplaysFromArchiveDir) {
    CaptureRun r;
    const std::string original = read_file(r.ob());
    const auto archive = archive_existing(
        r.in.path(), {kBtc.market_data, kBtc.orderbook, kBtc.snapshots}, "20261011T120000Z");
    ASSERT_TRUE(archive.has_value());
    std::string err;
    ASSERT_EQ(replay_capturing(options({*archive / kBtc.market_data}, r.out.path()), err), 0)
        << err;
    EXPECT_EQ(read_file(r.replayed()), original);
}

TEST(Replay, DuplicateBasenamesRejectedBeforeOutputChanges) {
    CaptureRun a;
    TempDir other;
    test::write_loss_scenario(other.path());
    write_file(a.replayed(), "existing output");
    std::string err;
    EXPECT_EQ(replay_capturing(options({a.md(), other / kBtc.market_data}, a.out.path()), err), 2);
    EXPECT_NE(err.find(a.md().string()), std::string::npos) << err;
    EXPECT_EQ(read_file(a.replayed()), "existing output");
    // The same file named twice (even through a different spelling) is replayed once.
    const fs::path again = a.in / "." / kBtc.market_data;
    EXPECT_EQ(replay_capturing(options({a.md(), again}, a.out.path()), err), 0) << err;
    EXPECT_EQ(read_file(a.replayed()), read_file(a.ob()));
}

// --- blc_verify ---------------------------------------------------------------------------------

VerifyResult verify(std::vector<fs::path> files, std::optional<fs::path> log = std::nullopt) {
    VerifyOptions o;
    o.files = std::move(files);
    o.epoch_log = std::move(log);
    return verify_files(o);
}

std::string all_errors(const VerifyResult& r) {
    std::string s;
    for (const auto& e : r.errors) {
        s += e + "\n";
    }
    return s;
}

// Replaces line `index` of `path` (0 = header) using `edit` and checks verify rejects it.
template <class Edit>
void expect_rejected(const fs::path& path, std::size_t index, Edit edit,
                     const std::vector<fs::path>& others, const char* what) {
    const std::string original = read_file(path);
    auto lines = split_lines(original);
    ASSERT_LT(index, lines.size()) << what;
    edit(lines[index]);
    write_file(path, join_lines(lines));
    std::vector<fs::path> files = others;
    files.push_back(path);
    const VerifyResult r = verify(files);
    EXPECT_FALSE(r.ok) << what;
    EXPECT_FALSE(r.errors.empty()) << what;
    write_file(path, original);
}

std::string replace_field(const std::string& line, std::size_t field, const std::string& value) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < field; ++i) {
        start = line.find(',', start) + 1;
    }
    std::size_t end = line.find(',', start);
    if (end == std::string::npos) {
        end = line.size();
    }
    return line.substr(0, start) + value + line.substr(end);
}

TEST(Verify, AcceptsCaptureOutput) {
    CaptureRun r;
    const VerifyResult res = verify({r.md(), r.ob(), r.sidecar()});
    EXPECT_TRUE(res.ok) << all_errors(res);
}

TEST(Verify, RejectsEachViolation) {
    CaptureRun r;
    const auto md = r.md();
    const auto ob = r.ob();
    const auto sc = r.sidecar();
    const auto set = [](std::size_t f, const char* v) {
        return [f, v](std::string& line) { line = replace_field(line, f, v); };
    };
    // Headers and shape.
    expect_rejected(
        md, 0, [](std::string& l) { l += ",extra"; }, {}, "md header");
    expect_rejected(
        ob, 2, [](std::string& l) { l += ",1"; }, {}, "ob column count");
    expect_rejected(
        md, 1, [](std::string& l) { l += "\"unterminated"; }, {}, "csv quoting");
    // Numeric ranges, signs, overflow, trailing characters.
    expect_rejected(md, 1, set(1, "1000000000"), {}, "tnsec range");
    expect_rejected(md, 1, set(4, "-1"), {}, "negative shard");
    expect_rejected(md, 1, set(4, "65536"), {}, "shard overflow");
    expect_rejected(md, 1, set(5, "-1"), {}, "negative epoch");
    expect_rejected(md, 1, set(5, "4294967296"), {}, "epoch overflow");
    expect_rejected(md, 1, set(6, "18446744073709551616"), {}, "conn_seq overflow");
    expect_rejected(md, 1, set(6, "0x"), {}, "trailing characters");
    expect_rejected(md, 1, set(0, "9223372036854775808"), {}, "tsec overflow");
    expect_rejected(ob, 1, set(3, "2147483648"), {}, "id overflow");
    expect_rejected(ob, 1, set(6, "-5"), {}, "negative price");
    // Market data.
    expect_rejected(md, 1, set(2, "coin"), {}, "venue");
    expect_rejected(md, 1, set(3, "aggTrade"), {}, "stream_kind");
    expect_rejected(md, 1, set(7, "btcusdt"), {}, "lower-case symbol");
    expect_rejected(md, 2, set(7, "ETHUSDT"), {}, "symbol changes");
    expect_rejected(md, 2, set(6, "0"), {}, "non-increasing conn_seq");
    expect_rejected(
        md, 1, [](std::string& l) { l = l.substr(0, l.size() - 2) + "\"\"\""; }, {},
        "invalid JSON");
    expect_rejected(
        md, 1, [](std::string& l) { l.insert(l.find("{") + 1, "\n"); }, {}, "payload newline");
    // Order book.
    expect_rejected(ob, 2, set(2, "5"), {}, "seqNo gap");
    expect_rejected(ob, 1, set(4, "X"), {}, "type");
    expect_rejected(ob, 1, set(5, "Q"), {}, "side");
    expect_rejected(ob, 1, set(4, "D"), {}, "first row not R");
    expect_rejected(ob, 1, set(7, "20000000000"), {}, "bids not descending");
    expect_rejected(ob, 1, set(17, "100"), {}, "asks not ascending");
    expect_rejected(ob, 1, set(11, "0"), {}, "price without size");
    expect_rejected(ob, 1, set(3, "7"), {}, "id changes");
    // Sidecar.
    expect_rejected(sc, 2, set(6, "0"), {}, "decreasing apply_before_row");
    expect_rejected(sc, 2, set(6, "999"), {md}, "apply_before_row beyond market rows");
}

std::string md_row(std::uint64_t shard, std::uint64_t epoch, std::uint64_t seq,
                   const char* symbol = "BTCUSDT") {
    return "1791669450,1,spot,trade," + std::to_string(shard) + "," + std::to_string(epoch) + "," +
           std::to_string(seq) + "," + symbol + ",\"{\"\"e\"\":\"\"trade\"\"}\"\n";
}

TEST(Verify, EpochLogAccounting) {
    TempDir d;
    const fs::path btc = d / "market_data_spot_BTCUSDT_2026-10-11.csv";
    const fs::path eth = d / "market_data_spot_ETHUSDT_2026-10-11.csv";
    const std::string header = std::string(kMarketDataHeader) + "\n";
    // Shard 0 carries both symbols; seq 2 of epoch 0 was malformed, epoch 1 lost 1 frame.
    write_file(btc, header + md_row(0, 0, 0) + md_row(0, 0, 3) + md_row(0, 1, 0));
    write_file(eth, header + md_row(0, 0, 1, "ETHUSDT") + md_row(0, 1, 1, "ETHUSDT"));
    const fs::path log = d / "stderr.log";
    const auto check = [&](const std::string& text) {
        write_file(log, text);
        return verify({btc, eth}, log);
    };
    const std::string e0 =
        "2026-10-11T12:00:00Z INFO epoch_closed shard=0 epoch=0 frames=4 "
        "malformed=1 drops=0\n";
    const std::string e1 =
        "2026-10-11T12:00:01Z INFO epoch_closed shard=0 epoch=1 frames=3 "
        "malformed=0 drops=1\n";
    VerifyResult ok = check("noise line\n" + e0 + e1);
    EXPECT_TRUE(ok.ok) << all_errors(ok);

    VerifyResult r = check(e0);  // epoch 1 has rows but no epoch_closed line
    EXPECT_FALSE(r.ok);
    EXPECT_NE(all_errors(r).find("shard=0 epoch=1"), std::string::npos) << all_errors(r);

    r = check(e0 + e0 + e1);  // duplicate line
    EXPECT_FALSE(r.ok);
    EXPECT_NE(all_errors(r).find("epoch=0"), std::string::npos) << all_errors(r);

    r = check(e0 + "epoch_closed shard=0 epoch=1 frames=4 malformed=0 drops=1\n");  // unaccounted
    EXPECT_FALSE(r.ok);
    EXPECT_NE(all_errors(r).find("shard=0 epoch=1"), std::string::npos) << all_errors(r);

    r = check("epoch_closed shard=0 epoch=0 frames=3 malformed=0 drops=0\n" + e1);  // seq 3 >= 3
    EXPECT_FALSE(r.ok);

    // Duplicate sequence numbers across symbol files.
    write_file(eth, header + md_row(0, 0, 0, "ETHUSDT") + md_row(0, 1, 1, "ETHUSDT"));
    r = check(e0 + e1);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(all_errors(r).find("duplicate"), std::string::npos) << all_errors(r);
}

TEST(Verify, EpochLogManyEpochs) {
    TempDir d;
    const fs::path btc = d / "market_data_spot_BTCUSDT_2026-10-11.csv";
    std::string md = std::string(kMarketDataHeader) + "\n";
    std::string log;
    for (std::uint64_t e = 0; e < 2000; ++e) {
        md += md_row(3, e, 0) + md_row(3, e, 1);
        log +=
            "epoch_closed shard=3 epoch=" + std::to_string(e) + " frames=3 malformed=0 drops=1\n";
    }
    write_file(btc, md);
    write_file(d / "log.txt", log);
    const VerifyResult r = verify({btc}, d / "log.txt");
    EXPECT_TRUE(r.ok) << all_errors(r);
}

TEST(Verify, DiagnosticsAreCapped) {
    TempDir d;
    const fs::path btc = d / "market_data_spot_BTCUSDT_2026-10-11.csv";
    std::string md = std::string(kMarketDataHeader) + "\n";
    for (int i = 0; i < 500; ++i) {
        md += "1791669450,1,coin,trade,0,0," + std::to_string(i) + ",BTCUSDT,\"{}\"\n";
    }
    write_file(btc, md);
    const VerifyResult r = verify({btc});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.errors.size(), 101u);
    EXPECT_NE(r.errors.back().find("400 more"), std::string::npos) << r.errors.back();
}

}  // namespace
}  // namespace blc
