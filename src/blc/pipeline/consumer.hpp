#pragma once

#include "blc/pipeline/metrics.hpp"
#include "blc/pipeline/processor.hpp"
#include "blc/pipeline/spsc_ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace blc {

struct ConsumerConfig {
    std::function<std::int64_t()> steady_now_ns;  // injectable clock
    std::int64_t started_at_ns = 0;               // process start on the same clock
    const IoMetrics* io = nullptr;                // process-lifetime counters; null offline
    std::chrono::milliseconds flush_every{1000};
    std::chrono::seconds stats_every{10};  // 0 disables periodic lines (the summary remains)
    std::function<void(const std::string&)> on_stats;
};

// The processing thread's body (design §6.7 steps 4-5): peek -> process -> pop; park on the ring
// when it is empty; flush and print stats on schedule. On a processor fatal error it stores 1
// into `fatal_code` (release) once, then keeps draining without writing. When the ring is empty
// and the producer has closed it, it drains once more and finishes the outputs (a failed finish
// stores 1). Prints a final "summary" line and returns the value of `fatal_code`.
int run_consumer(SpscRing& ring, Processor& processor, std::atomic<int>& fatal_code,
                 const ConsumerConfig& config) noexcept;

}  // namespace blc
