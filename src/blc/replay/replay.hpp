#pragma once

#include "blc/cli/options.hpp"

namespace blc {

// Replay mode (design §7): regenerates each market-data file's order book from it and its
// sidecar, with the same processor capture uses. Returns 0 on success, 1 if any input failed,
// 2 for usage errors (nothing replayable, or two inputs that would write the same output).
int run_replay(const ReplayOptions& options);

}  // namespace blc
