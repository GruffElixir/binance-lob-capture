#pragma once

#include <cstdint>

namespace blc::test {

// The test binary replaces the global operator new/delete family (counting_allocator.cpp).
// Allocations are counted only on the thread that armed counting, and not while suspended
// (the logger suspends counting around its own formatting through set_log_allocation_hook).
void arm_allocation_counting() noexcept;
std::uint64_t disarm_allocation_counting() noexcept;  // returns the count since arming
void suspend_allocation_counting(bool suspend) noexcept;

}  // namespace blc::test
