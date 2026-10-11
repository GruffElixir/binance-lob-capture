#include "blc/version.hpp"

#include <algorithm>

#include <gtest/gtest.h>

namespace {

TEST(Scaffold, VersionIsMajorMinorPatch) {
    const auto dots = std::count(blc::kVersion.begin(), blc::kVersion.end(), '.');
    EXPECT_EQ(dots, 2);
}

}  // namespace
