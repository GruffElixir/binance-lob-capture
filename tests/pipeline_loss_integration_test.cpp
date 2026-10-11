#include "blc/core/time.hpp"
#include "blc/core/types.hpp"
#include "blc/pipeline/output_files.hpp"
#include "support/loss_scenario.hpp"
#include "support/temp_dir.hpp"

#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

std::vector<std::vector<std::string>> rows(const std::string& text) {
    std::vector<std::vector<std::string>> out;
    std::istringstream in(text);
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::istringstream ls(line);
        std::string cell;
        while (std::getline(ls, cell, ',')) {
            f.push_back(cell);
        }
        out.push_back(f);
    }
    return out;
}

TEST(LossIntegration, DroppedDiffThenTradeEmitsNoStaleRow) {
    test::TempDir dir;
    test::write_loss_scenario(dir.path());
    if (::testing::Test::HasFailure()) {
        return;
    }
    const FileNames names = file_names(Venue::Spot, "BTCUSDT", "2026-10-11");

    // The withheld trade never reached the market-data file.
    const std::string md = test::read_file(dir / names.market_data);
    EXPECT_EQ(md.find(R"(""t"":)" + std::to_string(test::kLossWithheldTradeId) + ","),
              std::string::npos);
    EXPECT_NE(md.find(R"(""t"":1000,)"), std::string::npos);

    const auto ob = rows(test::read_file(dir / names.orderbook));
    ASSERT_GE(ob.size(), 4u);
    const std::string stale_bid = std::to_string(100 * kPriceScale);
    std::size_t after_drop = 0;
    for (const auto& r : ob) {
        ASSERT_EQ(r.size(), 26u);
        const std::int64_t ns = std::stoll(r[0]) * kNanosPerSecond + std::stoll(r[1]);
        if (ns < test::kLossDropRecvNs) {
            continue;
        }
        ++after_drop;
        for (std::size_t c = 6; c <= 10; ++c) {
            EXPECT_NE(r[c], stale_bid) << "row seqNo " << r[2] << " shows the deleted best bid";
        }
    }
    // After the drop: the resync R row (best bid 99) and the epoch-1 trade.
    ASSERT_EQ(after_drop, 2u);
    const auto& resync = ob[ob.size() - 2];
    EXPECT_EQ(resync[4], "R");
    EXPECT_EQ(resync[6], std::to_string(99 * kPriceScale));
    EXPECT_EQ(resync[16], std::to_string(101 * kPriceScale));
    EXPECT_EQ(resync[22], std::to_string(3 * kPriceScale));  // ask 102 qty 3 from the epoch-1 diff
    EXPECT_EQ(ob.back()[4], "T");
    // seqNo stays contiguous across the reset.
    for (std::size_t i = 0; i < ob.size(); ++i) {
        EXPECT_EQ(ob[i][2], std::to_string(i));
    }
}

}  // namespace
}  // namespace blc
