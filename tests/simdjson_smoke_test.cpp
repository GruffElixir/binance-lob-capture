#include <cstdint>
#include <simdjson.h>
#include <string_view>

#include <gtest/gtest.h>

namespace {

TEST(SimdjsonSmoke, OnDemandReadsInteger) {
    simdjson::ondemand::parser parser;
    const simdjson::padded_string json(std::string_view(R"({"a":1})"));
    auto doc = parser.iterate(json);
    std::int64_t a = 0;
    ASSERT_EQ(doc["a"].get_int64().get(a), simdjson::SUCCESS);
    EXPECT_EQ(a, 1);
}

}  // namespace
