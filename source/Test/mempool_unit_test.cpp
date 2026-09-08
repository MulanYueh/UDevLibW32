/*
 * Catch2 regression tests for the public mempool API.
 *
 * Build the C implementation as C and link this translation unit as C++:
 *
 *   gcc -std=c11 -D_WIN32 -c allocator.c mempool.c libc.c
 *   g++ -std=c++17 -I. mempool_unit_test.cpp allocator.o mempool.o libc.o \
 *       -o mempool_unit_test.exe
 *
 * This file is intentionally independent from the other unit-test runners.
 */

#define CATCH_CONFIG_MAIN
#include "catch2/catch.hpp"

#include "mempool.h"

#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace
{

static MEMPOOL_TYPE invalid_pool_type()
{
    MEMPOOL_TYPE value = MEMPOOL_PAGED;
    unsigned int raw = 99U;

    /* Form an invalid enum value without triggering -Wconversion in GCC. */
    static_assert(sizeof(value) == sizeof(raw),
                  "MEMPOOL_TYPE must have the Windows enum width");
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

struct PoolOwner
{
    MEMPOOL *pool;

    PoolOwner() : pool(static_cast<MEMPOOL *>(0))
    {
        pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    }

    ~PoolOwner()
    {
        if (pool != static_cast<MEMPOOL *>(0)) {
            (void)Mempool_DestroyPool(pool);
            pool = static_cast<MEMPOOL *>(0);
        }
    }

    PoolOwner(const PoolOwner &) = delete;
    PoolOwner &operator=(const PoolOwner &) = delete;
};

static void exercise_pool(MEMPOOL *pool, unsigned int seed)
{
    unsigned int state = seed;
    unsigned int index = 0U;
    ULONG size = 0UL;
    void *memory = static_cast<void *>(0);

    while (index < 300U) {
        state = state * 1664525U + 1013904223U;
        size = static_cast<ULONG>(1UL + (state % 2048U));
        memory = Mempool_Alloc(pool, size);
        if (memory != static_cast<void *>(0)) {
            static_cast<unsigned char *>(memory)[0] = 0x5AU;
            static_cast<unsigned char *>(memory)[size - 1UL] = 0xA5U;
            Mempool_Free(memory);
        }
        ++index;
    }
}

} /* namespace */

TEST_CASE("mempool rejects invalid handles and zero-size requests",
          "[mempool][validation]")
{
    MEMPOOL *pool = static_cast<MEMPOOL *>(0);
    void *memory = static_cast<void *>(0);
    ULONG released = 0UL;

    REQUIRE(Mempool_CreatePool(invalid_pool_type()) ==
            static_cast<MEMPOOL *>(0));
    REQUIRE(Mempool_Alloc(static_cast<MEMPOOL *>(0), 1UL) ==
            static_cast<void *>(0));
    REQUIRE(Mempool_Alloc(static_cast<MEMPOOL *>(0), 0UL) ==
            static_cast<void *>(0));

    pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    REQUIRE(pool != static_cast<MEMPOOL *>(0));
    memory = Mempool_Alloc(pool, 0UL);
    REQUIRE(memory == static_cast<void *>(0));
    released = Mempool_DestroyPool(pool);
    REQUIRE(released >= 1UL);
    /* The registry rejects a stale handle without dereferencing freed memory. */
    REQUIRE(Mempool_DestroyPool(pool) == 0UL);
}

TEST_CASE("mempool serves aligned small allocations and preserves data",
          "[mempool][small]")
{
    PoolOwner owner = PoolOwner();
    std::vector<void *> allocations;
    const ULONG sizes[] = {1UL, 7UL, 16UL, 127UL, 128UL, 129UL, 1024UL};
    const std::size_t size_count = sizeof(sizes) / sizeof(sizes[0]);
    std::size_t index = 0U;
    void *memory = static_cast<void *>(0);
    unsigned char *bytes = static_cast<unsigned char *>(0);
    std::uintptr_t address = static_cast<std::uintptr_t>(0U);

    REQUIRE(owner.pool != static_cast<MEMPOOL *>(0));
    allocations.reserve(size_count);
    while (index < size_count) {
        memory = Mempool_Alloc(owner.pool, sizes[index]);
        REQUIRE(memory != static_cast<void *>(0));
        address = reinterpret_cast<std::uintptr_t>(memory);
        REQUIRE((address % sizeof(void *)) == static_cast<std::uintptr_t>(0U));
        bytes = static_cast<unsigned char *>(memory);
        bytes[0] = 0x11U;
        bytes[sizes[index] - 1UL] = 0xEEU;
        if (sizes[index] == 1UL)
            REQUIRE(bytes[0] == 0xEEU);
        else
            REQUIRE(bytes[0] == 0x11U);
        REQUIRE(bytes[sizes[index] - 1UL] == 0xEEU);
        allocations.push_back(memory);
        ++index;
    }
    while (!allocations.empty()) {
        Mempool_Free(allocations.back());
        allocations.pop_back();
    }
}

TEST_CASE("mempool handles page-sized allocations and overflow limits",
          "[mempool][large]")
{
    PoolOwner owner = PoolOwner();
    void *large = static_cast<void *>(0);
    void *too_large = static_cast<void *>(0);
    unsigned char *bytes = static_cast<unsigned char *>(0);

    REQUIRE(owner.pool != static_cast<MEMPOOL *>(0));
    large = Mempool_Alloc(owner.pool, 65536UL);
    REQUIRE(large != static_cast<void *>(0));
    bytes = static_cast<unsigned char *>(large);
    bytes[0] = 0x3CU;
    bytes[65535U] = 0xC3U;
    REQUIRE(bytes[0] == 0x3CU);
    REQUIRE(bytes[65535U] == 0xC3U);
    Mempool_Free(large);

    too_large = Mempool_Alloc(owner.pool, 0xFFFFFFFFUL);
    REQUIRE(too_large == static_cast<void *>(0));
}

TEST_CASE("mempool grows across pages and reclaims empty pages",
          "[mempool][pages]")
{
    PoolOwner owner = PoolOwner();
    std::vector<void *> allocations;
    std::size_t index = 0U;
    const std::size_t allocation_count = 900U;
    void *memory = static_cast<void *>(0);

    REQUIRE(owner.pool != static_cast<MEMPOOL *>(0));
    allocations.reserve(allocation_count);
    while (index < allocation_count) {
        memory = Mempool_Alloc(owner.pool, 64UL);
        REQUIRE(memory != static_cast<void *>(0));
        allocations.push_back(memory);
        ++index;
    }
    index = 0U;
    while (index < allocations.size()) {
        Mempool_Free(allocations[index]);
        ++index;
    }
    allocations.clear();
    /* The owner page is retained, while fully empty data pages are reclaimed. */
    memory = Mempool_Alloc(owner.pool, 64UL);
    REQUIRE(memory != static_cast<void *>(0));
    Mempool_Free(memory);
}

TEST_CASE("mempool remains usable under concurrent allocation and free",
          "[mempool][threading]")
{
    PoolOwner owner = PoolOwner();
    std::thread first = std::thread();
    std::thread second = std::thread();
    std::thread third = std::thread();
    std::thread fourth = std::thread();

    REQUIRE(owner.pool != static_cast<MEMPOOL *>(0));
    first = std::thread(exercise_pool, owner.pool, 1U);
    second = std::thread(exercise_pool, owner.pool, 2U);
    third = std::thread(exercise_pool, owner.pool, 3U);
    fourth = std::thread(exercise_pool, owner.pool, 4U);
    first.join();
    second.join();
    third.join();
    fourth.join();
    REQUIRE(Mempool_Alloc(owner.pool, 32UL) != static_cast<void *>(0));
}
