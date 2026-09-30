// services/planegcs/build/capi/gcs_capi_alloc.cpp
//
// What it is: the allocator the C API build links in place of the default
// `operator new`: it hands back zeroed memory.
//
// Why: planegcs reads a few members it never initialises, and what it reads
// there is whatever the block held the last time it was used. A fresh wasm
// memory is zero, so the first system solved in an instance saw zeros; the
// tenth saw the leftovers of the nine before it, and the same sketch solved
// differently. Zeroing at allocation makes every solve see what the first one
// saw, wherever the block comes from.
//
// Who links it: the `planegcs-capi` target in ../planegcs/CMakeLists.txt only.
// What does NOT belong here: any other allocation policy.

#include <cstdlib>
#include <cstring>
#include <new>

void* operator new(std::size_t n) {
    void* p = std::calloc(n == 0 ? 1 : n, 1);
    if (p == nullptr) {
        std::abort();
    }
    return p;
}

void* operator new[](std::size_t n) {
    return operator new(n);
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return std::calloc(n == 0 ? 1 : n, 1);
}

void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return std::calloc(n == 0 ? 1 : n, 1);
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
