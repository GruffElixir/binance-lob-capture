#pragma once

#include "blc/core/types.hpp"

#include <cstdint>

namespace blc {

enum class SeqClass { Stale, Gap, Continues };

// Classifies a diff (U, u, and pu on usdm) against reference id `ref` (design §5.4). The same
// rule is used for the window, for a Synced book and when replaying the window.
//   spot: stale if u <= ref; gap if U > ref + 1; otherwise continues (overlap accepted).
//   usdm: stale if u <= ref; gap if pu != ref;   otherwise continues.
SeqClass classify(Venue venue, std::uint64_t U, std::uint64_t u, std::uint64_t pu,
                  std::uint64_t ref) noexcept;

// True when a snapshot with lastUpdateId L already contains the diff ending at u.
//   spot: u <= L;  usdm: u < L.
bool contained_in_snapshot(Venue venue, std::uint64_t u, std::uint64_t L) noexcept;

// True when the diff (U, u) is the first one to apply after a snapshot with lastUpdateId L.
//   spot: U <= L + 1 <= u;  usdm: U <= L <= u.
bool bridges(Venue venue, std::uint64_t U, std::uint64_t u, std::uint64_t L) noexcept;

}  // namespace blc
