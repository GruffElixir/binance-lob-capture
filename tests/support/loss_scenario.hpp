#pragma once

#include <cstdint>
#include <filesystem>

namespace blc::test {

// The design §13 'loss' scenario, end to end without a network: frames go through FrameRouter
// into a deliberately tiny ring and a Processor writing real files for spot BTCUSDT
// (date 2026-10-11) into `dir`.
//
// Epoch 0 syncs (best bid 100), then the ring is filled so that a diff deleting the best bid is
// dropped (the loss), and a trade arriving next is withheld. Epoch 1 resyncs from a snapshot
// whose best bid is 99, then a trade emits a T row.
void write_loss_scenario(const std::filesystem::path& dir);

inline constexpr std::int64_t kLossT0 = 1791669450'000'000'000;
inline constexpr std::int64_t kLossStepNs = 1'000'000;
// Receive time of the dropped diff: no order-book row at or after it may show bid 100.
inline constexpr std::int64_t kLossDropRecvNs = kLossT0 + 1000 * kLossStepNs;
inline constexpr std::uint64_t kLossWithheldTradeId = 999;

}  // namespace blc::test
