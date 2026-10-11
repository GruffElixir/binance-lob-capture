#include "blc/core/time.hpp"

#include <gtest/gtest.h>

namespace blc {
namespace {

TEST(SplitNs, ExactSecondHasZeroNanos) {
    EXPECT_EQ(split_ns(1'791'669'451'000'000'000).nsec, 0);
    EXPECT_EQ(split_ns(1'791'669'451'000'000'000).sec, 1'791'669'451);
}

TEST(SplitNs, Remainder) {
    const auto t = split_ns(1'791'669'451'812'345'678);
    EXPECT_EQ(t.sec, 1'791'669'451);
    EXPECT_EQ(t.nsec, 812'345'678);
}

TEST(SplitNs, NegativeFloors) {
    const auto t = split_ns(-1);
    EXPECT_EQ(t.sec, -1);
    EXPECT_EQ(t.nsec, 999'999'999);
}

TEST(UtcDate, Boundaries) {
    EXPECT_EQ(utc_date(0), "1970-01-01");
    EXPECT_EQ(utc_date(951'782'400'000'000'000), "2000-02-29");  // leap day
    EXPECT_EQ(utc_date(4'107'542'400'000'000'000), "2100-03-01");
    EXPECT_EQ(utc_date(1'791'676'799'999'999'999), "2026-10-10");  // last ns of 2026-10-10
    EXPECT_EQ(utc_date(1'791'676'800'000'000'000), "2026-10-11");  // first ns of 2026-10-11
    EXPECT_EQ(utc_compact_stamp(1'791'669'451'000'000'000), "20261010T215731Z");
}

TEST(WallClock, IsAfter2025) {
    EXPECT_GT(wall_clock_ns(), 1'735'689'600'000'000'000);
}

}  // namespace
}  // namespace blc
