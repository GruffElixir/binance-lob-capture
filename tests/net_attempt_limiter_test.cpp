#include "blc/net/attempt_limiter.hpp"

#include <chrono>

#include <gtest/gtest.h>

namespace blc {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST(AttemptLimiter, WindowOf200) {
    AttemptLimiter limiter;
    const Clock::time_point t0{};
    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(limiter.try_take(t0 + std::chrono::seconds(i))) << i;
    }
    EXPECT_FALSE(limiter.try_take(t0 + 200s));
    EXPECT_EQ(limiter.next_available(t0 + 200s), t0 + 5min);
    EXPECT_FALSE(limiter.try_take(t0 + 5min - 1ms));
    EXPECT_TRUE(limiter.try_take(t0 + 5min));  // the oldest attempt has left the window
    EXPECT_FALSE(limiter.try_take(t0 + 5min));
    EXPECT_TRUE(limiter.try_take(t0 + 5min + 1s));
}

TEST(AttemptLimiter, SmallWindowForTests) {
    AttemptLimiter limiter(2, 1s);
    const Clock::time_point t0{};
    EXPECT_EQ(limiter.next_available(t0), t0);
    EXPECT_TRUE(limiter.try_take(t0));
    EXPECT_TRUE(limiter.try_take(t0));
    EXPECT_FALSE(limiter.try_take(t0 + 500ms));
    EXPECT_EQ(limiter.next_available(t0 + 500ms), t0 + 1s);
    EXPECT_TRUE(limiter.try_take(t0 + 1s));
}

TEST(Backoff, DoublesToTheCapAndResets) {
    Backoff b(500ms, 30s, 0.0, 1);
    const long long expected[] = {500, 1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (const long long ms : expected) {
        EXPECT_EQ(b.next().count(), ms);
    }
    b.reset();
    EXPECT_EQ(b.next().count(), 500);
}

TEST(Backoff, JitterStaysWithinBounds) {
    Backoff b(500ms, 30s, 0.2, 7);
    long long base = 500;
    bool varied = false;
    for (int i = 0; i < 12; ++i) {
        const long long v = b.next().count();
        EXPECT_GE(v, base * 8 / 10) << i;
        EXPECT_LE(v, base * 12 / 10) << i;
        varied = varied || v != base;
        base = std::min<long long>(base * 2, 30000);
    }
    EXPECT_TRUE(varied);
}

}  // namespace
}  // namespace blc
