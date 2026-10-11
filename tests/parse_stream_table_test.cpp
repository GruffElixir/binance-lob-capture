#include "blc/core/symbols.hpp"
#include "blc/parse/stream_table.hpp"

#include <string>

#include <gtest/gtest.h>

namespace blc {
namespace {

TEST(StreamTable, LookupAndList) {
    const auto symbols = parse_symbol_list(Venue::Spot, "BTCUSDT,ETHUSDT");
    const StreamTable table(symbols, 3);

    auto e = table.find("ethusdt@depth5@100ms");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->symbol_index, 4);
    EXPECT_EQ(e->kind, StreamKind::Depth5);

    e = table.find("ethusdt@depth@100ms");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->symbol_index, 4);
    EXPECT_EQ(e->kind, StreamKind::DepthDiff);

    e = table.find("btcusdt@trade");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->symbol_index, 3);
    EXPECT_EQ(e->kind, StreamKind::Trade);

    EXPECT_FALSE(table.find("btcusdt@aggTrade").has_value());
    EXPECT_FALSE(table.find("BTCUSDT@trade").has_value());
    EXPECT_FALSE(table.find("").has_value());

    std::string list;
    table.append_stream_list(list);
    EXPECT_EQ(list,
              "btcusdt@depth@100ms/btcusdt@depth5@100ms/btcusdt@trade/"
              "ethusdt@depth@100ms/ethusdt@depth5@100ms/ethusdt@trade");
}

}  // namespace
}  // namespace blc
