#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>
namespace blc::bench {
using Clock = std::chrono::steady_clock;
inline constexpr std::size_t kIterations = 1'000'000;
inline double elapsed_ns(Clock::time_point start) {
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}
inline void report(const std::string& name, std::vector<double> repetitions) {
    std::sort(repetitions.begin(), repetitions.end());
    std::cout << name << " median_ns/op=" << (repetitions[4] + repetitions[5]) / 2
              << " p99_repetition_ns/op=" << repetitions.back() << '\n';
}
void book();
void payload();
void decimal();
void ring();
void replay(const std::string& input);
}  // namespace blc::bench
