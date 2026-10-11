#include "blc/pipeline/spsc_ring.hpp"

#include <cstring>
#include <random>
#include <simdjson.h>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace blc {
namespace {

RecordHeader header_for(std::uint64_t seq) {
    RecordHeader h{};
    h.kind = RecordKind::Market;
    h.stream = StreamKind::Trade;
    h.symbol = static_cast<std::uint16_t>(seq % 7);
    h.shard = 1;
    h.epoch = 3;
    h.conn_seq = seq;
    h.recv_ns = static_cast<std::int64_t>(seq) * 10;
    h.steady_ns = static_cast<std::int64_t>(seq) * 20;
    return h;
}

std::string payload_for(std::uint64_t seq, std::size_t len) {
    std::string s(len, '\0');
    for (std::size_t i = 0; i < len; ++i) {
        s[i] = static_cast<char>('a' + (seq + i) % 26);
    }
    return s;
}

void expect_record(SpscRing& ring, std::uint64_t seq, std::size_t len) {
    RecordView v;
    ASSERT_TRUE(ring.try_peek(v)) << seq;
    EXPECT_EQ(v.header.conn_seq, seq);
    EXPECT_EQ(v.header.recv_ns, static_cast<std::int64_t>(seq) * 10);
    EXPECT_EQ(v.header.payload_len, len);
    ASSERT_EQ(v.payload.size, len);
    EXPECT_GE(v.payload.capacity, len + simdjson::SIMDJSON_PADDING);
    EXPECT_EQ(std::string(v.payload.data, v.payload.size), payload_for(seq, len));
    volatile char sink = 0;
    for (std::size_t i = 0; i < simdjson::SIMDJSON_PADDING; ++i) {
        sink = static_cast<char>(sink + v.payload.data[len + i]);  // padding is readable
    }
    (void)sink;
    ring.pop();
}

TEST(SpscRing, CapacityBands) {
    EXPECT_EQ(SpscRing::capacity_for_symbols(1), 16u << 20);
    EXPECT_EQ(SpscRing::capacity_for_symbols(64), 16u << 20);
    EXPECT_EQ(SpscRing::capacity_for_symbols(65), 32u << 20);
    EXPECT_EQ(SpscRing::capacity_for_symbols(128), 32u << 20);
    EXPECT_EQ(SpscRing::capacity_for_symbols(256), 64u << 20);
}

TEST(SpscRing, EmptyPeekFails) {
    SpscRing ring(4096);
    RecordView v;
    EXPECT_FALSE(ring.try_peek(v));
    EXPECT_EQ(ring.capacity(), 4096u);
}

TEST(SpscRing, WrapCases) {
    SpscRing ring(4096);
    const std::size_t per_record = SpscRing::record_bytes(832);  // 960 bytes
    ASSERT_EQ(4096 % 64, 0u);
    // Records that end exactly at the end of the buffer (no wrap marker needed).
    std::uint64_t seq = 0;
    const std::size_t exact = 4096 / per_record;
    for (std::size_t i = 0; i < exact; ++i) {
        ASSERT_TRUE(ring.try_push(header_for(seq), payload_for(seq, 832)));
        ++seq;
    }
    for (std::uint64_t s = 0; s < seq; ++s) {
        expect_record(ring, s, 832);
    }
    // A record that does not fit before the end: needs a wrap marker.
    for (int round = 0; round < 10; ++round) {
        const std::size_t len = 300 + static_cast<std::size_t>(round) * 97;
        ASSERT_TRUE(ring.try_push(header_for(seq), payload_for(seq, len)));
        expect_record(ring, seq, len);
        ++seq;
    }
    // A record larger than half the ring, pushed into an empty ring.
    SpscRing big(4096);
    ASSERT_TRUE(big.try_push(header_for(1), payload_for(1, 2500)));
    expect_record(big, 1, 2500);
    EXPECT_FALSE(big.try_push(header_for(2), payload_for(2, 5000)));  // can never fit
}

TEST(SpscRing, FullRejectsWithoutCorruption) {
    SpscRing ring(4096);
    std::uint64_t pushed = 0;
    while (ring.try_push(header_for(pushed), payload_for(pushed, 200))) {
        ++pushed;
    }
    ASSERT_GT(pushed, 5u);
    EXPECT_FALSE(ring.try_push(header_for(pushed), payload_for(pushed, 200)));
    expect_record(ring, 0, 200);
    ASSERT_TRUE(ring.try_push(header_for(pushed), payload_for(pushed, 200)));
    for (std::uint64_t s = 1; s <= pushed; ++s) {
        expect_record(ring, s, 200);
    }
    RecordView v;
    EXPECT_FALSE(ring.try_peek(v));
    EXPECT_GT(ring.high_water(), 3000u);
}

TEST(SpscRing, CloseProtocol) {
    SpscRing ring(4096);
    ASSERT_TRUE(ring.try_push(header_for(0), "a"));
    ASSERT_TRUE(ring.try_push(header_for(1), "bb"));
    EXPECT_FALSE(ring.producer_closed());
    ring.close_producer();
    EXPECT_TRUE(ring.producer_closed());
    std::uint64_t got = 0;
    RecordView v;
    while (ring.try_peek(v)) {
        EXPECT_EQ(v.header.conn_seq, got);
        ring.pop();
        ++got;
    }
    EXPECT_EQ(got, 2u);
    ring.wait(std::chrono::milliseconds(10));  // closed: returns promptly
}

TEST(SpscRing, RandomLengthFuzz) {
    SpscRing ring(1u << 16);
    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> len(0, 3000);
    std::uniform_int_distribution<int> batch(1, 12);
    std::uint64_t next_push = 0;
    std::uint64_t next_pop = 0;
    std::vector<std::size_t> lengths;
    while (next_pop < 100'000) {
        const int n = batch(rng);
        for (int i = 0; i < n && next_push < 100'000; ++i) {
            const std::size_t l = len(rng);
            if (!ring.try_push(header_for(next_push), payload_for(next_push, l))) {
                break;
            }
            lengths.push_back(l);
            ++next_push;
        }
        const int m = batch(rng);
        for (int i = 0; i < m && next_pop < next_push; ++i) {
            expect_record(ring, next_pop, lengths[next_pop]);
            ++next_pop;
        }
    }
}

TEST(SpscRing, TwoThreadStress) {
    SpscRing ring(1u << 16);
    constexpr std::uint64_t kCount = 1'000'000;
    std::thread producer([&] {
        std::mt19937 rng(7);
        std::uniform_int_distribution<std::size_t> len(0, 600);
        for (std::uint64_t s = 0; s < kCount;) {
            const std::size_t l = len(rng);
            const std::string p = payload_for(s, l);
            while (!ring.try_push(header_for(s), p)) {
                std::this_thread::yield();
            }
            ++s;
        }
        ring.close_producer();
    });
    std::uint64_t expected = 0;
    bool ok = true;
    for (;;) {
        RecordView v;
        if (ring.try_peek(v)) {
            if (v.header.conn_seq != expected || std::string(v.payload.data, v.payload.size) !=
                                                     payload_for(expected, v.payload.size)) {
                ok = false;
            }
            ring.pop();
            ++expected;
            continue;
        }
        if (ring.producer_closed()) {
            if (!ring.try_peek(v)) {
                break;
            }
            continue;
        }
        ring.wait(std::chrono::milliseconds(50));
    }
    producer.join();
    EXPECT_TRUE(ok);
    EXPECT_EQ(expected, kCount);
}

}  // namespace
}  // namespace blc
