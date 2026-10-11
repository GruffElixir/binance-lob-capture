#include "blc/core/decimal.hpp"

#include <cstdint>

#include <gtest/gtest.h>

namespace blc {
namespace {

TEST(Decimal, SpecTable) {
    std::int64_t v{};
    EXPECT_EQ(parse_e8("83044.00000000", v), DecimalError::Ok);
    EXPECT_EQ(v, 8304400000000);
    EXPECT_EQ(parse_e8("3.25216000", v), DecimalError::Ok);
    EXPECT_EQ(v, 325216000);
    EXPECT_EQ(parse_e8("0.00009000", v), DecimalError::Ok);
    EXPECT_EQ(v, 9000);
    EXPECT_EQ(parse_e8("83000.10", v), DecimalError::Ok);
    EXPECT_EQ(v, 8300010000000);
    EXPECT_EQ(parse_e8("0.022", v), DecimalError::Ok);
    EXPECT_EQ(v, 2200000);
    EXPECT_EQ(parse_e8("0", v), DecimalError::Ok);
    EXPECT_EQ(v, 0);
    EXPECT_EQ(parse_e8("0.000000010", v), DecimalError::Ok);
    EXPECT_EQ(v, 1);
}

TEST(Decimal, Rejects) {
    std::int64_t v{};
    for (const char* s : {"", ".5", "5.", "-1", "+1", "1e5", "1,0", " 1", "1 ", "1..2", "0x1"}) {
        EXPECT_NE(parse_e8(s, v), DecimalError::Ok) << s;
    }
    EXPECT_EQ(parse_e8("0.000000001", v), DecimalError::TooPrecise);
    EXPECT_EQ(parse_e8("", v), DecimalError::Empty);
    EXPECT_EQ(parse_e8(".5", v), DecimalError::InvalidChar);
    EXPECT_EQ(parse_e8("5.", v), DecimalError::InvalidChar);
}

TEST(Decimal, OverflowBoundary) {
    std::int64_t v{};
    EXPECT_EQ(parse_e8("92233720368.54775807", v), DecimalError::Ok);
    EXPECT_EQ(v, INT64_MAX);
    EXPECT_EQ(parse_e8("92233720368.54775808", v), DecimalError::Overflow);
    EXPECT_EQ(parse_e8("100000000000", v), DecimalError::Overflow);
    EXPECT_EQ(parse_e8("99999999999999999999999", v), DecimalError::Overflow);
}

TEST(Decimal, LongZeroTailsAndLeadingZeros) {
    std::int64_t v{};
    EXPECT_EQ(parse_e8("000001.10000000000000000000", v), DecimalError::Ok);
    EXPECT_EQ(v, 110000000);
}

}  // namespace
}  // namespace blc
