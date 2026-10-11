#include "blc/net/dispatch_policy.hpp"

#include <chrono>
#include <optional>

#include <gtest/gtest.h>

namespace blc {
namespace {

using namespace std::chrono_literals;
using Clock = DispatchPolicy::Clock;

const Clock::time_point T0 = Clock::time_point{} + 1000s;
constexpr std::int64_t kWall = 1791669420;  // a whole minute (UTC)

std::uint64_t slot(std::uint32_t epoch, std::uint32_t number) {
    return (std::uint64_t{epoch} << 32) | number;
}

HttpResult ok(int used_weight = 0) {
    HttpResult r;
    r.status = 200;
    r.body = "{}";
    r.used_weight_1m = used_weight;
    return r;
}

HttpResult status(int code, int retry_after = 0, std::optional<std::int64_t> err = std::nullopt) {
    HttpResult r;
    r.status = code;
    r.retry_after_s = retry_after;
    r.binance_error_code = err;
    return r;
}

HttpResult transport_failure() {
    HttpResult r;
    r.transport_error = true;
    return r;
}

// Dispatches, answers 200 and delivers: the slot is free again.
Dispatch complete_one(DispatchPolicy& p, Clock::time_point now) {
    const auto d = p.next_dispatch(now, kWall);
    EXPECT_TRUE(d.has_value());
    if (!d) {
        return {};
    }
    EXPECT_EQ(p.on_response(d->symbol, ok(), now, kWall), Outcome::Deliver);
    p.on_delivery(d->symbol, DeliveryResult::Delivered, now);
    return *d;
}

TEST(DispatchPolicy, WeightsAndBudgets) {
    DispatchPolicy spot(Venue::Spot, NetTuning{});
    DispatchPolicy usdm(Venue::Usdm, NetTuning{});
    EXPECT_EQ(spot.budget_per_minute(), 3000);
    EXPECT_EQ(usdm.budget_per_minute(), 1200);
    spot.want(RequestKind::Validation, 0, 0, T0);
    spot.want(RequestKind::Snapshot, 1, slot(0, 1), T0);
    usdm.want(RequestKind::Validation, 0, 0, T0);
    usdm.want(RequestKind::Snapshot, 1, slot(0, 1), T0);
    auto a = spot.next_dispatch(T0, kWall);
    auto b = spot.next_dispatch(T0, kWall);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->kind, RequestKind::Validation);
    EXPECT_EQ(a->weight, 2);
    EXPECT_EQ(b->kind, RequestKind::Snapshot);
    EXPECT_EQ(b->weight, 50);
    EXPECT_EQ(b->slot_value, slot(0, 1));
    a = usdm.next_dispatch(T0, kWall);
    b = usdm.next_dispatch(T0, kWall);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->weight, 1);
    EXPECT_EQ(b->weight, 20);
}

TEST(DispatchPolicy, TokenPacing) {
    DispatchPolicy p(Venue::Spot, NetTuning{});
    for (std::uint16_t s = 0; s < 61; ++s) {
        p.want(RequestKind::Snapshot, s, slot(0, 1), T0);
    }
    for (int i = 0; i < 60; ++i) {
        EXPECT_EQ(complete_one(p, T0).symbol, i);
    }
    EXPECT_FALSE(p.next_dispatch(T0, kWall).has_value());  // the bucket is empty
    EXPECT_EQ(p.next_wakeup(T0), T0 + 1s);                 // 50 weight refills in 1 s
    EXPECT_FALSE(p.next_dispatch(T0 + 999ms, kWall).has_value());
    EXPECT_EQ(complete_one(p, T0 + 1s).symbol, 60);
    EXPECT_EQ(p.next_wakeup(T0 + 1s), Clock::time_point::max());
}

TEST(DispatchPolicy, FourDispatchSlots) {
    DispatchPolicy p(Venue::Spot, NetTuning{});
    for (std::uint16_t s = 0; s < 5; ++s) {
        p.want(RequestKind::Snapshot, s, slot(0, 1), T0);
    }
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(p.next_dispatch(T0, kWall).has_value());
    }
    EXPECT_FALSE(p.next_dispatch(T0, kWall).has_value());
    EXPECT_EQ(p.on_response(0, ok(), T0, kWall), Outcome::Deliver);
    EXPECT_FALSE(p.next_dispatch(T0, kWall).has_value());  // the body still holds its slot
    p.on_delivery(0, DeliveryResult::Delivered, T0);
    EXPECT_TRUE(p.next_dispatch(T0, kWall).has_value());
}

TEST(DispatchPolicy, UsedWeightHeaderPausesUntilNextMinute) {
    DispatchPolicy p(Venue::Spot, NetTuning{});
    p.want(RequestKind::Snapshot, 0, slot(0, 1), T0);
    p.want(RequestKind::Snapshot, 1, slot(0, 1), T0);
    const auto d = p.next_dispatch(T0, kWall);
    ASSERT_TRUE(d);
    // 2960 used: less than one request's weight (50) below the 3000 budget.
    EXPECT_EQ(p.on_response(0, ok(2960), T0, kWall + 45), Outcome::Deliver);
    p.on_delivery(0, DeliveryResult::Delivered, T0);
    EXPECT_FALSE(p.next_dispatch(T0 + 14s, kWall + 59).has_value());
    EXPECT_EQ(p.next_wakeup(T0), T0 + 15s);
    EXPECT_TRUE(p.next_dispatch(T0 + 15s, kWall + 60).has_value());
}

TEST(DispatchPolicy, Status429CoolsDownAndRequeuesAtHead) {
    DispatchPolicy p(Venue::Spot, NetTuning{});
    p.want(RequestKind::Snapshot, 0, slot(0, 1), T0);
    p.want(RequestKind::Snapshot, 1, slot(0, 1), T0);
    ASSERT_EQ(p.next_dispatch(T0, kWall)->symbol, 0);
    EXPECT_EQ(p.on_response(0, status(429, 5), T0, kWall), Outcome::Retry);
    EXPECT_TRUE(p.cooling_down(T0));
    EXPECT_FALSE(p.next_dispatch(T0 + 4s, kWall).has_value());
    EXPECT_EQ(p.next_wakeup(T0), T0 + 5s);
    const auto d = p.next_dispatch(T0 + 5s, kWall);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->symbol, 0);  // requeued at the head, before symbol 1
}

TEST(DispatchPolicy, Status418CoolsDown) {
    DispatchPolicy p(Venue::Spot, NetTuning{});
    p.want(RequestKind::Snapshot, 0, slot(0, 1), T0);
    ASSERT_TRUE(p.next_dispatch(T0, kWall));
    EXPECT_EQ(p.on_response(0, status(418, 120), T0, kWall), Outcome::Retry);
    EXPECT_FALSE(p.next_dispatch(T0 + 119s, kWall).has_value());
    ASSERT_TRUE(p.next_dispatch(T0 + 120s, kWall));
    // Without Retry-After: the default 60 s.
    EXPECT_EQ(p.on_response(0, status(418), T0 + 120s, kWall), Outcome::Retry);
    EXPECT_FALSE(p.next_dispatch(T0 + 179s, kWall).has_value());
    EXPECT_TRUE(p.next_dispatch(T0 + 180s, kWall).has_value());
}

TEST(DispatchPolicy, ServerErrorRetriedWithBackoff) {
    DispatchPolicy p(Venue::Spot, NetTuning{});
    p.want(RequestKind::Snapshot, 0, slot(0, 1), T0);
    ASSERT_TRUE(p.next_dispatch(T0, kWall));
    EXPECT_EQ(p.on_response(0, status(503), T0, kWall), Outcome::Retry);
    EXPECT_FALSE(p.next_dispatch(T0 + 499ms, kWall).has_value());
    EXPECT_EQ(p.next_wakeup(T0), T0 + 500ms);
    ASSERT_TRUE(p.next_dispatch(T0 + 500ms, kWall));
    EXPECT_EQ(p.on_response(0, transport_failure(), T0 + 500ms, kWall), Outcome::Retry);
    EXPECT_FALSE(p.next_dispatch(T0 + 1499ms, kWall).has_value());  // doubled: 1 s
    EXPECT_TRUE(p.next_dispatch(T0 + 1500ms, kWall).has_value());
}

TEST(DispatchPolicy, FourXxByKind) {
    DispatchPolicy p(Venue::Spot, NetTuning{});
    p.want(RequestKind::Validation, 0, 0, T0);
    p.want(RequestKind::Validation, 1, 0, T0);
    p.want(RequestKind::Snapshot, 2, slot(0, 1), T0);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(p.next_dispatch(T0, kWall));
    }
    EXPECT_EQ(p.on_response(0, status(400, 0, -1121), T0, kWall), Outcome::InvalidSymbol);
    EXPECT_EQ(p.on_response(1, status(403), T0, kWall), Outcome::Fatal);
    EXPECT_EQ(p.on_response(2, status(400, 0, -1121), T0, kWall), Outcome::Fatal);
}

TEST(DispatchPolicy, NewerValueReplacesQueuedInPlace) {
    DispatchPolicy p(Venue::Spot, NetTuning{});
    p.want(RequestKind::Snapshot, 1, slot(0, 1), T0);
    p.want(RequestKind::Snapshot, 2, slot(0, 1), T0);
    p.want(RequestKind::Snapshot, 1, slot(1, 2), T0);
    const auto a = p.next_dispatch(T0, kWall);
    const auto b = p.next_dispatch(T0, kWall);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->symbol, 1);
    EXPECT_EQ(a->slot_value, slot(1, 2));
    EXPECT_EQ(b->symbol, 2);
    EXPECT_FALSE(p.next_dispatch(T0, kWall).has_value());
}

TEST(DispatchPolicy, NewerValueDuringFlightServedAfter) {
    DispatchPolicy p(Venue::Spot, NetTuning{});
    p.want(RequestKind::Snapshot, 3, slot(0, 1), T0);
    ASSERT_EQ(p.next_dispatch(T0, kWall)->slot_value, slot(0, 1));
    p.want(RequestKind::Snapshot, 3, slot(0, 2), T0);
    EXPECT_FALSE(p.next_dispatch(T0, kWall).has_value());  // one in flight per symbol
    EXPECT_EQ(p.on_response(3, ok(), T0, kWall), Outcome::Deliver);
    p.on_delivery(3, DeliveryResult::Delivered, T0);
    const auto d = p.next_dispatch(T0, kWall);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->slot_value, slot(0, 2));
}

TEST(DispatchPolicy, FailedRequestWithNewerValueRequeuesOnlyNewer) {
    DispatchPolicy p(Venue::Spot, NetTuning{});
    p.want(RequestKind::Snapshot, 3, slot(0, 1), T0);
    ASSERT_TRUE(p.next_dispatch(T0, kWall));
    p.want(RequestKind::Snapshot, 3, slot(1, 2), T0);
    EXPECT_EQ(p.on_response(3, status(500), T0, kWall), Outcome::Retry);
    const auto d = p.next_dispatch(T0 + 1s, kWall);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->slot_value, slot(1, 2));
    EXPECT_EQ(p.on_response(3, ok(), T0 + 1s, kWall), Outcome::Deliver);
    p.on_delivery(3, DeliveryResult::Delivered, T0 + 1s);
    EXPECT_FALSE(p.next_dispatch(T0 + 1h, kWall).has_value());  // nothing else queued
}

TEST(DispatchPolicy, RingFullKeepsSlotDiscardReleases) {
    DispatchPolicy p(Venue::Spot, NetTuning{});
    for (std::uint16_t s = 0; s < 5; ++s) {
        p.want(RequestKind::Snapshot, s, slot(0, 1), T0);
    }
    for (std::uint16_t s = 0; s < 4; ++s) {
        ASSERT_TRUE(p.next_dispatch(T0, kWall));
        EXPECT_EQ(p.on_response(s, ok(), T0, kWall), Outcome::Deliver);
        p.on_delivery(s, DeliveryResult::RingFull, T0);
    }
    EXPECT_FALSE(p.next_dispatch(T0, kWall).has_value());  // four bodies hold all slots
    p.on_delivery(2, DeliveryResult::RingFull, T0 + 10ms);
    EXPECT_FALSE(p.next_dispatch(T0 + 10ms, kWall).has_value());
    p.on_delivery(2, DeliveryResult::Discard, T0 + 20ms);
    const auto d = p.next_dispatch(T0 + 20ms, kWall);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->symbol, 4);
}

TEST(DispatchPolicy, ValidationUnreachableAfterGiveUp) {
    DispatchPolicy p(Venue::Spot, NetTuning{});
    p.want(RequestKind::Validation, 0, 0, T0);
    Clock::time_point now = T0;
    Outcome last = Outcome::Retry;
    for (int i = 0; i < 100 && last == Outcome::Retry; ++i) {
        const auto d = p.next_dispatch(now, kWall);
        if (!d) {
            now = p.next_wakeup(now);
            continue;
        }
        last = p.on_response(0, transport_failure(), now, kWall);
    }
    EXPECT_EQ(last, Outcome::Unreachable);
    EXPECT_GE(now - T0, 30s);
    EXPECT_LT(now - T0, 60s);
}

}  // namespace
}  // namespace blc
