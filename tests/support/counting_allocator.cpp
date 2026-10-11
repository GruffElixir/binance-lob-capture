#include "support/counting_allocator.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace blc::test {

namespace {

thread_local bool t_armed = false;
thread_local int t_suspended = 0;
thread_local std::uint64_t t_count = 0;

void note_allocation() noexcept {
    if (t_armed && t_suspended == 0) {
        ++t_count;
    }
}

void* allocate(std::size_t n) noexcept {
    note_allocation();
    return std::malloc(n == 0 ? 1 : n);
}

void* allocate_aligned(std::size_t n, std::size_t align) noexcept {
    note_allocation();
    if (n == 0) {
        n = 1;
    }
#ifdef _WIN32
    return _aligned_malloc(n, align);
#else
    void* p = nullptr;
    if (align < sizeof(void*)) {
        align = sizeof(void*);
    }
    return posix_memalign(&p, align, n) == 0 ? p : nullptr;
#endif
}

void release_aligned(void* p) noexcept {
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}

}  // namespace

void arm_allocation_counting() noexcept {
    t_count = 0;
    t_suspended = 0;
    t_armed = true;
}

std::uint64_t disarm_allocation_counting() noexcept {
    t_armed = false;
    return t_count;
}

void suspend_allocation_counting(bool suspend) noexcept {
    t_suspended += suspend ? 1 : -1;
}

}  // namespace blc::test

// --- Replacements of the global allocation functions --------------------------------------------

void* operator new(std::size_t n) {
    if (void* p = blc::test::allocate(n)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    if (void* p = blc::test::allocate(n)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return blc::test::allocate(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return blc::test::allocate(n);
}
void* operator new(std::size_t n, std::align_val_t a) {
    if (void* p = blc::test::allocate_aligned(n, static_cast<std::size_t>(a))) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) {
    if (void* p = blc::test::allocate_aligned(n, static_cast<std::size_t>(a))) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return blc::test::allocate_aligned(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return blc::test::allocate_aligned(n, static_cast<std::size_t>(a));
}

void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    blc::test::release_aligned(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    blc::test::release_aligned(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    blc::test::release_aligned(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    blc::test::release_aligned(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    blc::test::release_aligned(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    blc::test::release_aligned(p);
}
