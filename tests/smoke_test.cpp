#include <gtest/gtest.h>

#include <algorithm>

#include "blc/version.hpp"

namespace {

TEST(Scaffold, VersionIsMajorMinorPatch) {
    const auto dots = std::count(blc::kVersion.begin(), blc::kVersion.end(), '.');
    EXPECT_EQ(dots, 2);
}

}  // namespace
