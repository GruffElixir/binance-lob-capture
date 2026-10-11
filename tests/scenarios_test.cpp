#include "blc/cli/options.hpp"
#include "blc/replay/replay.hpp"
#include "blc/verify/verify.hpp"
#include "support/fixtures.hpp"
#include "support/temp_dir.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

namespace fs = std::filesystem;

// Hand-derived scenarios (tests/scenarios/<name>/README.md explains each expected row).
const char* const kScenarios[] = {
    "spot_sync_overlap",
    "usdm_bridge_boundary",
    "gap_resync",
    "two_symbols_one_shard_reconnect",
    "coverage_exhaustion",
    "depth5_mismatch",
    "depth5_beyond_coverage",
    "oversized_diff",
    "sync_by_later_diff",
    "trade_side",
};

std::set<std::string> file_names_in(const fs::path& dir) {
    std::set<std::string> names;
    for (const auto& e : fs::directory_iterator(dir)) {
        names.insert(e.path().filename().string());
    }
    return names;
}

class Scenarios : public ::testing::TestWithParam<const char*> {};

TEST_P(Scenarios, ReplayMatchesHandDerivedExpectation) {
    const fs::path root =
        fs::path(test::fixture_path("")).parent_path().parent_path() / "scenarios" / GetParam();
    ASSERT_TRUE(fs::is_directory(root / "input")) << root;
    ASSERT_TRUE(fs::is_directory(root / "expected")) << root;
    ASSERT_TRUE(fs::exists(root / "README.md")) << root;

    std::vector<fs::path> inputs;
    for (const auto& e : fs::directory_iterator(root / "input")) {
        const std::string name = e.path().filename().string();
        if (name.rfind("market_data_", 0) == 0) {
            inputs.push_back(e.path());
        }
    }
    std::sort(inputs.begin(), inputs.end());
    ASSERT_FALSE(inputs.empty());

    // Every input file is replayed alone, as the design's independence rule requires.
    test::TempDir out;
    for (const fs::path& input : inputs) {
        ReplayOptions o;
        o.inputs = {input};
        o.output_dir = out.path();
        ASSERT_EQ(run_replay(o), 0) << input;
    }
    EXPECT_EQ(file_names_in(out.path()), file_names_in(root / "expected"));
    for (const auto& name : file_names_in(root / "expected")) {
        EXPECT_EQ(test::read_file(out / name), test::read_file(root / "expected" / name))
            << GetParam() << "/" << name;
    }

    // Inputs and outputs satisfy the CSV contracts.
    VerifyOptions v;
    for (const auto& e : fs::directory_iterator(root / "input")) {
        v.files.push_back(e.path());
    }
    for (const auto& e : fs::directory_iterator(out.path())) {
        v.files.push_back(e.path());
    }
    const VerifyResult r = verify_files(v);
    std::string errors;
    for (const auto& e : r.errors) {
        errors += e + "\n";
    }
    EXPECT_TRUE(r.ok) << errors;
}

INSTANTIATE_TEST_SUITE_P(All, Scenarios, ::testing::ValuesIn(kScenarios),
                         [](const ::testing::TestParamInfo<const char*>& param_info) {
                             return std::string(param_info.param);
                         });

}  // namespace
}  // namespace blc
