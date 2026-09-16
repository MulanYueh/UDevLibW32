/*
 * Catch2 regression tests for map.c and its allocator integration.
 *
 * Build map.c and allocator.c as C, then link this file as C++:
 *
 *   gcc -std=c11 -I. -c map.c allocator.c
 *   g++ -std=c++17 -I. map_unit_test.cpp map.o allocator.o -o map_unit_test.exe
 */

#define CATCH_CONFIG_MAIN
#include "catch2/catch.hpp"

#include "allocator.h"
#include "map.h"

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{

struct AllocationState
{
    std::size_t allocations = 0U;
    std::size_t frees = 0U;
    bool fail = false;
};

void* sentinel_pointer(std::uintptr_t value)
{
    return reinterpret_cast<void*>(value);
}

void* counting_alloc(void* context, std::size_t size)
{
    AllocationState* state = static_cast<AllocationState*>(context);

    if (state == nullptr || state->fail || size == 0U) {
        return nullptr;
    }
    void* memory = std::malloc(size);
    if (memory != nullptr) {
        ++state->allocations;
    }
    return memory;
}

void counting_free(void* context, void* pointer)
{
    AllocationState* state = static_cast<AllocationState*>(context);

    if (pointer != nullptr) {
        std::free(pointer);
        if (state != nullptr) {
            ++state->frees;
        }
    }
}

int c_string_size(const void* key)
{
    if (key == nullptr) {
        return 0;
    }
    return static_cast<int>(std::strlen(static_cast<const char*>(key)) + 1U);
}

unsigned int c_string_hash(const void* key, std::size_t size)
{
    const unsigned char* bytes = static_cast<const unsigned char*>(key);
    unsigned int hash = 2166136261U;

    while (size != 0U) {
        hash = (hash ^ *bytes++) * 16777619U;
        --size;
    }
    return hash;
}

map_bool_t c_string_match(const void* left, const void* right)
{
    if (left == nullptr || right == nullptr) {
        return left == right ? MAP_TRUE : MAP_FALSE;
    }
    return std::strcmp(static_cast<const char*>(left),
                       static_cast<const char*>(right)) == 0
               ? MAP_TRUE
               : MAP_FALSE;
}

void configure_c_string_keys(map_base_t* map)
{
    REQUIRE(map_set_key_functions(map, c_string_size, c_string_hash,
                                  c_string_match) != MAP_FALSE);
}

void populate_string_map(map_base_t* map, int count)
{
    char key[32];

    configure_c_string_keys(map);
    for (int index = 0; index < count; ++index) {
        std::snprintf(key, sizeof(key), "key-%d", index);
        REQUIRE(map_insert(map, key, &index, sizeof(index)) != nullptr);
    }
}

} /* namespace */

TEST_CASE("allocator supplies usable blocks and rejects zero size",
          "[allocator]")
{
    CHECK(Allocator_Malloc(0U) == nullptr);

    unsigned char* memory =
        static_cast<unsigned char*>(Allocator_Malloc(32U));
    REQUIRE(memory != nullptr);
    for (std::size_t index = 0U; index < 32U; ++index) {
        memory[index] = static_cast<unsigned char>(index);
    }
    CHECK(memory[0] == 0U);
    CHECK(memory[31] == 31U);
    Allocator_Free(memory);
}

TEST_CASE("map initializes, rejects invalid resize requests and clears",
          "[map][lifecycle]")
{
    map_base_t map = {};

    map_init(&map, nullptr);
    CHECK(map.buckets == nullptr);
    CHECK(map.nbuckets == 0);
    CHECK(map.nnodes == 0);
    CHECK(map_resize(&map, 0) == MAP_FALSE);
    CHECK(map_resize(&map, -1) == MAP_FALSE);
    CHECK(map_get(&map, sentinel_pointer(1U)) == nullptr);

    map_clear(&map);
    CHECK(map.buckets == nullptr);
    CHECK(map.nbuckets == 0);
    CHECK(map.nnodes == 0);
}

TEST_CASE("map copies values and supports case-insensitive wide keys",
          "[map][keys]")
{
    map_base_t map = {};
    int value = 42;
    int zero_value;
    int taken = 0;
    const wchar_t key[] = L"Answer";

    map_init(&map, nullptr);
    REQUIRE(map_set_key_functions(&map, map_wcssize, map_wcsihash,
                                  map_wcsimatch) != MAP_FALSE);

    REQUIRE(map_insert(&map, key, &value, sizeof(value)) != nullptr);
    CHECK(*static_cast<int*>(map_get(&map, L"answer")) == 42);
    CHECK(map_get(&map, L"missing") == nullptr);

    zero_value = 9;
    REQUIRE(map_insert(&map, L"zero", nullptr, sizeof(zero_value)) !=
            nullptr);
    CHECK(*static_cast<int*>(map_get(&map, L"ZERO")) == 0);
    CHECK(map_wcssize(nullptr) == 0);
    CHECK(map_wcsimatch(nullptr, nullptr) == MAP_TRUE);
    CHECK(map_wcsimatch(nullptr, L"x") == MAP_FALSE);

    REQUIRE(map_take(&map, L"ANSWER", &taken, sizeof(taken)) != MAP_FALSE);
    CHECK(taken == 42);
    CHECK(map_get(&map, key) == nullptr);
    CHECK(map.nnodes == 1);

    map_clear(&map);
}

TEST_CASE("pointer-key wide string helpers match without copying pointees",
          "[map][keys][wide]")
{
    map_base_t map = {};
    int value = 7;
    const wchar_t* stored = L"Alpha";
    const wchar_t* lookup = L"alpha";

    map_init(&map, nullptr);
    REQUIRE(map_set_key_functions(&map, nullptr, str_map_hash,
                                  str_map_match) != MAP_FALSE);
    REQUIRE(map_insert(&map, stored, &value, sizeof(value)) != nullptr);
    REQUIRE(map_get(&map, lookup) != nullptr);
    CHECK(*static_cast<int*>(map_get(&map, lookup)) == 7);
    map_clear(&map);
}

TEST_CASE("insert and append preserve duplicate-key ordering",
          "[map][duplicates]")
{
    map_base_t map = {};
    int first = 1;
    int second = 2;
    int third = 3;
    int taken = 0;

    map_init(&map, nullptr);
    REQUIRE(map_insert(&map, sentinel_pointer(1U), &first,
                       sizeof(first)) != nullptr);
    REQUIRE(map_append(&map, sentinel_pointer(1U), &second,
                       sizeof(second)) != nullptr);
    CHECK(*static_cast<int*>(map_get(&map, sentinel_pointer(1U)))
          == 1);

    REQUIRE(map_insert(&map, sentinel_pointer(1U), &third,
                       sizeof(third)) != nullptr);
    CHECK(*static_cast<int*>(map_get(&map, sentinel_pointer(1U)))
          == 3);

    REQUIRE(map_take(&map, sentinel_pointer(1U), &taken,
                     sizeof(taken)) != MAP_FALSE);
    CHECK(taken == 3);
    REQUIRE(map_take(&map, sentinel_pointer(1U), &taken,
                     sizeof(taken)) != MAP_FALSE);
    CHECK(taken == 1);
    REQUIRE(map_take(&map, sentinel_pointer(1U), &taken,
                     sizeof(taken)) != MAP_FALSE);
    CHECK(taken == 2);
    CHECK(map_take(&map, sentinel_pointer(1U), &taken,
                   sizeof(taken)) == MAP_FALSE);
    map_clear(&map);
}

TEST_CASE("map expands, rehashes arbitrary bucket counts and finds all keys",
          "[map][resize]")
{
    map_base_t map = {};
    constexpr int key_count = 256;
    char key[32];

    map_init(&map, nullptr);
    populate_string_map(&map, key_count);
    REQUIRE(map.nnodes == key_count);
    REQUIRE(map.nbuckets > 0);

    REQUIRE(map_resize(&map, 17) != MAP_FALSE);
    CHECK(map.nbuckets == 17);
    for (int index = 0; index < key_count; ++index) {
        std::snprintf(key, sizeof(key), "key-%d", index);
        REQUIRE(map_get(&map, key) != nullptr);
        CHECK(*static_cast<int*>(map_get(&map, key)) == index);
    }
    map_clear(&map);
}

TEST_CASE("map_take checks copied value capacity and external pointers",
          "[map][values]")
{
    map_base_t map = {};
    int value = 123;
    int output = 0;
    std::uint32_t oversized_output[2] = {};
    void* external = sentinel_pointer(0x1234U);
    void* taken_external = nullptr;

    map_init(&map, nullptr);
    REQUIRE(map_insert(&map, sentinel_pointer(1U), &value,
                       sizeof(value)) != nullptr);
    CHECK(map_take(&map, sentinel_pointer(1U),
                   oversized_output, sizeof(oversized_output)) == MAP_FALSE);
    CHECK(map_get(&map, sentinel_pointer(1U)) != nullptr);
    CHECK(map_take(&map, sentinel_pointer(1U), &taken_external,
                   0U) == MAP_FALSE);
    CHECK(map_take(&map, sentinel_pointer(1U), &output,
                   sizeof(output)) != MAP_FALSE);
    CHECK(output == value);

    REQUIRE(map_insert(&map, sentinel_pointer(2U), external, 0U)
            == external);
    REQUIRE(map_take(&map, sentinel_pointer(2U),
                     &taken_external, 0U) != MAP_FALSE);
    CHECK(taken_external == external);
    map_clear(&map);
}

TEST_CASE("iterators enumerate, filter and erase every matching node",
          "[map][iterator]")
{
    map_base_t map = {};
    map_iter_t iterator;
    int seen = 0;

    map_init(&map, nullptr);
    populate_string_map(&map, 64);
    iterator = map_iter();
    while (map_next(&map, &iterator) != MAP_FALSE) {
        ++seen;
        CHECK(iterator.value != nullptr);
        (void)map_erase(&map, &iterator);
    }
    CHECK(seen == 64);
    CHECK(map.nnodes == 0);
    map_clear(&map);

    map_init(&map, nullptr);
    configure_c_string_keys(&map);
    int first = 1;
    int second = 2;
    int other = 3;
    REQUIRE(map_insert(&map, "same", &first, sizeof(first)) != nullptr);
    REQUIRE(map_append(&map, "same", &second, sizeof(second)) != nullptr);
    REQUIRE(map_insert(&map, "other", &other, sizeof(other)) != nullptr);

    iterator = map_key_iter(&map, "same");
    seen = 0;
    while (map_next(&map, &iterator) != MAP_FALSE) {
        ++seen;
        CHECK(*static_cast<int*>(iterator.value) == (seen == 1 ? 1 : 2));
        (void)map_erase(&map, &iterator);
    }
    CHECK(seen == 2);
    CHECK(map.nnodes == 1);
    CHECK(map_get(&map, "other") != nullptr);
    map_clear(&map);
}

TEST_CASE("custom allocator failures do not create partial map entries",
          "[map][allocator]")
{
    map_base_t map = {};
    AllocationState state;
    int value = 9;

    map_init(&map, nullptr);
    REQUIRE(map_set_allocator(&map, &state, counting_alloc, counting_free) !=
            MAP_FALSE);
    CHECK(map_set_allocator(&map, &state, counting_alloc, nullptr) ==
          MAP_FALSE);

    state.fail = true;
    CHECK(map_insert(&map, sentinel_pointer(1U), &value,
                     sizeof(value)) == nullptr);
    CHECK(map.nnodes == 0);
    CHECK(map.buckets == nullptr);

    state.fail = false;
    REQUIRE(map_insert(&map, sentinel_pointer(1U), &value,
                       sizeof(value)) != nullptr);
    CHECK(state.allocations >= 2U);

    map_node_t** buckets_before = map.buckets;
    const int bucket_count_before = map.nbuckets;
    state.fail = true;
    CHECK(map_resize(&map, bucket_count_before * 2) == MAP_FALSE);
    CHECK(map.buckets == buckets_before);
    CHECK(map.nbuckets == bucket_count_before);
    CHECK(map_get(&map, sentinel_pointer(1U)) != nullptr);

    state.fail = false;
    map_clear(&map);
    CHECK(state.allocations == state.frees);
}

TEST_CASE("map rejects allocator/key callback changes after insertion",
          "[map][configuration]")
{
    map_base_t map = {};
    AllocationState state;
    int value = 1;

    map_init(&map, nullptr);
    REQUIRE(map_insert(&map, sentinel_pointer(1U), &value,
                       sizeof(value)) != nullptr);
    CHECK(map_set_allocator(&map, &state, counting_alloc, counting_free) ==
          MAP_FALSE);
    CHECK(map_set_key_functions(&map, c_string_size, c_string_hash,
                                c_string_match) == MAP_FALSE);
    map_clear(&map);
}
