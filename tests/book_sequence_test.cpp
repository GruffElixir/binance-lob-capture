#include "blc/book/sequence.hpp"

#include <gtest/gtest.h>

namespace blc {
namespace {

TEST(Sequence, SpotOverlapContinues) {
    EXPECT_EQ(classify(Venue::Spot, 103, 108, 0, 105), SeqClass::Continues);
}

TEST(Sequence, SpotBoundaries) {
    EXPECT_EQ(classify(Venue::Spot, 106, 110, 0, 105), SeqClass::Continues);
    EXPECT_EQ(classify(Venue::Spot, 107, 110, 0, 105), SeqClass::Gap);
    EXPECT_EQ(classify(Venue::Spot, 100, 105, 0, 105), SeqClass::Stale);
    EXPECT_EQ(classify(Venue::Spot, 106, 106, 0, 105), SeqClass::Continues);
}

TEST(Sequence, UsdmUsesPu) {
    EXPECT_EQ(classify(Venue::Usdm, 120, 130, 105, 105), SeqClass::Continues);
    EXPECT_EQ(classify(Venue::Usdm, 120, 130, 104, 105), SeqClass::Gap);
}

TEST(Sequence, UsdmStaleWinsOverWrongPu) {
    EXPECT_EQ(classify(Venue::Usdm, 90, 105, 1, 105), SeqClass::Stale);
    EXPECT_EQ(classify(Venue::Usdm, 90, 100, 99, 105), SeqClass::Stale);
}

TEST(Sequence, Bridges) {
    EXPECT_TRUE(bridges(Venue::Spot, 101, 105, 100));
    EXPECT_TRUE(bridges(Venue::Spot, 95, 101, 100));
    EXPECT_FALSE(bridges(Venue::Spot, 102, 105, 100));
    EXPECT_FALSE(bridges(Venue::Spot, 90, 100, 100));
    EXPECT_TRUE(bridges(Venue::Usdm, 95, 105, 100));
    EXPECT_TRUE(bridges(Venue::Usdm, 100, 100, 100));
    EXPECT_FALSE(bridges(Venue::Usdm, 101, 105, 100));
    EXPECT_FALSE(bridges(Venue::Usdm, 90, 99, 100));
    EXPECT_TRUE(contained_in_snapshot(Venue::Spot, 100, 100));
    EXPECT_FALSE(contained_in_snapshot(Venue::Spot, 101, 100));
    EXPECT_FALSE(contained_in_snapshot(Venue::Usdm, 100, 100));
    EXPECT_TRUE(contained_in_snapshot(Venue::Usdm, 99, 100));
}

}  // namespace
}  // namespace blc
