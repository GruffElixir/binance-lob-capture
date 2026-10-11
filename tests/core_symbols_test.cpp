#include "blc/core/errors.hpp"
#include "blc/core/symbols.hpp"

#include <string>

#include <gtest/gtest.h>

namespace blc {
namespace {

TEST(Symbols, NormalisesSpacesAndCase) {
    const auto s = parse_symbol_list(Venue::Spot, " btcusdt , ETHusdt ");
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s[0].upper, "BTCUSDT");
    EXPECT_EQ(s[0].lower, "btcusdt");
    EXPECT_EQ(s[0].id, 2014473836);
    EXPECT_EQ(s[1].upper, "ETHUSDT");
    EXPECT_EQ(s[1].id, 1884648048);
}

TEST(Symbols, UsdmIdsDiffer) {
    const auto s = parse_symbol_list(Venue::Usdm, "BTCUSDT");
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].id, 1567771867);
}

TEST(Symbols, Rejects) {
    for (const char* l : {"", "BTCUSDT,", "BTCUSDT,,ETHUSDT", "B", "BTC-USDT", "BTCUSDT,btcusdt",
                          "ABCDEFGHIJKLMNOPQRSTU", " , "}) {
        EXPECT_THROW(parse_symbol_list(Venue::Spot, l), UsageError) << l;
    }
}

TEST(Symbols, EmptyItemMessageNamesPosition) {
    try {
        parse_symbol_list(Venue::Spot, "BTCUSDT,");
        FAIL();
    } catch (const UsageError& e) {
        EXPECT_NE(std::string(e.what()).find("empty"), std::string::npos) << e.what();
    }
}

TEST(Symbols, LimitOf256) {
    std::string list;
    for (int i = 0; i < 257; ++i) {
        list += (i == 0 ? "" : ",") + std::string("S") + std::to_string(i);
    }
    EXPECT_THROW(parse_symbol_list(Venue::Spot, list), UsageError);
    list = list.substr(0, list.rfind(','));
    EXPECT_EQ(parse_symbol_list(Venue::Spot, list).size(), 256u);
}

}  // namespace
}  // namespace blc
