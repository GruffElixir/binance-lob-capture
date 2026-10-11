#include "blc/net/capture_app.hpp"

#include <gtest/gtest.h>

TEST(Budget, IncludesAllFixedAndNetworkStorage) {
    EXPECT_EQ(blc::budget_mib(1, 1), 84u);
    EXPECT_EQ(blc::budget_mib(256, 64), 429u);
    EXPECT_EQ(blc::budget_mib(256, 6), 353u);
}
