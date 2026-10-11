#include "blc/core/errors.hpp"
#include "blc/core/instrument_id.hpp"
#include "blc/core/types.hpp"

#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

TEST(InstrumentId, SpecValues) {
    EXPECT_EQ(instrument_id(Venue::Spot, "BTCUSDT"), 2014473836);
    EXPECT_EQ(instrument_id(Venue::Usdm, "BTCUSDT"), 1567771867);
    EXPECT_EQ(instrument_id(Venue::Spot, "ETHUSDT"), 1884648048);
}

TEST(InstrumentId, Fnv1aIsConstexpr) {
    static_assert(fnv1a32("") == 2166136261u, "FNV-1a offset basis");
    EXPECT_EQ(fnv1a32("spot:BTCUSDT"), 2014473836u);
}

TEST(Types, VenueAndKindStrings) {
    EXPECT_EQ(to_string(StreamKind::DepthDiff), "depth_diff");
    EXPECT_EQ(to_string(StreamKind::Depth5), "depth5");
    EXPECT_EQ(to_string(StreamKind::Trade), "trade");
    EXPECT_EQ(to_string(Venue::Spot), "spot");
    EXPECT_EQ(to_string(Venue::Usdm), "usdm");
    EXPECT_EQ(parse_venue("usdm"), Venue::Usdm);
    EXPECT_EQ(parse_venue("spot"), Venue::Spot);
    EXPECT_FALSE(parse_venue("USDM").has_value());
}

TEST(Types, LevelSpanOfVector) {
    const std::vector<Level> v{{1, 2}, {3, 4}};
    const LevelSpan s = span_of(v);
    ASSERT_EQ(s.size, 2u);
    EXPECT_EQ(s.data[1], (Level{3, 4}));
}

TEST(Types, ErrorsAreRuntimeErrors) {
    EXPECT_THROW(throw UsageError("x"), std::runtime_error);
    EXPECT_THROW(throw IoError("y"), std::runtime_error);
}

}  // namespace
}  // namespace blc
