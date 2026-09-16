/*
 * Catch2 regression tests for the standalone circle queue.
 *
 * Build the queue and allocator implementations as C, then link this file
 * as C++ with the repository-local Catch2 v2 header:
 *
 *   gcc -std=c11 -I. -c circle_queue.c allocator.c libc.c
 *   g++ -std=c++17 -I. circle_queue_unit_test.cpp \
 *       circle_queue.o allocator.o libc.o -o circle_queue_unit_test.exe
 */

#define CATCH_CONFIG_MAIN
#include "catch2/catch.hpp"

#include "circle_queue.h"

#include <array>
#include <cstdint>
#include <deque>

namespace
{

struct QueueOwner
{
    CIRCLE_QUEUE_CONTEXT context;

    QueueOwner() : context()
    {
        context.ptrItems = static_cast<PCIRCLE_QUEUE_ITEM>(0);
        context.item_cnt = 0U;
        context.start_offset = 0U;
        context.end_offset = 0U;
        context.count = 0U;
        context.inited = CIRCLE_QUEUE_FALSE;
    }

    ~QueueOwner()
    {
        if (context.inited != CIRCLE_QUEUE_FALSE) {
            (void)Interface_CircleQueue_Destroy(&context);
        }
    }

    QueueOwner(const QueueOwner &) = delete;
    QueueOwner &operator=(const QueueOwner &) = delete;
};

static CIRCLE_QUEUE_ITEM item_for(std::uint8_t *data, std::uint32_t length)
{
    CIRCLE_QUEUE_ITEM item;
    item.length = length;
    item.lpData = data;
    return item;
}

static void require_item_equal(const CIRCLE_QUEUE_ITEM &actual,
                               const CIRCLE_QUEUE_ITEM &expected)
{
    REQUIRE(actual.length == expected.length);
    REQUIRE(actual.lpData == expected.lpData);
}

} /* namespace */

TEST_CASE("circle queue validates lifecycle and invalid arguments",
          "[circle_queue][validation]")
{
    CIRCLE_QUEUE_CONTEXT context = {};
    CIRCLE_QUEUE_ITEM item = {};
    std::uint8_t byte = 0xA5U;

    item = item_for(&byte, 1U);

    REQUIRE(Interface_CircleQueue_Create(static_cast<PCIRCLE_QUEUE_CONTEXT>(0),
                                         1U) == CIRCLE_QUEUE_FALSE);
    REQUIRE(Interface_CircleQueue_Create(&context, 0U) == CIRCLE_QUEUE_FALSE);
    REQUIRE(Interface_CircleQueue_IsEmpty(static_cast<const CIRCLE_QUEUE_CONTEXT *>(0)) ==
            CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Count(static_cast<const CIRCLE_QUEUE_CONTEXT *>(0)) ==
            0U);
    REQUIRE(Interface_CircleQueue_Capacity(static_cast<const CIRCLE_QUEUE_CONTEXT *>(0)) ==
            0U);
    REQUIRE(Interface_CircleQueue_Destroy(static_cast<PCIRCLE_QUEUE_CONTEXT>(0)) ==
            CIRCLE_QUEUE_FALSE);
    REQUIRE(Interface_CircleQueue_Push(static_cast<PCIRCLE_QUEUE_CONTEXT>(0), &item) ==
            CIRCLE_QUEUE_FALSE);
    REQUIRE(Interface_CircleQueue_Pop(static_cast<PCIRCLE_QUEUE_CONTEXT>(0), &item) ==
            CIRCLE_QUEUE_FALSE);

    REQUIRE(Interface_CircleQueue_Create(&context, 3U) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Create(&context, 3U) == CIRCLE_QUEUE_FALSE);
    REQUIRE(Interface_CircleQueue_Push(&context, static_cast<const CIRCLE_QUEUE_ITEM *>(0)) ==
            CIRCLE_QUEUE_FALSE);
    REQUIRE(Interface_CircleQueue_Push(&context, &item) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Push(&context, &item) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Destroy(&context) == CIRCLE_QUEUE_TRUE);
    REQUIRE(context.ptrItems == static_cast<PCIRCLE_QUEUE_ITEM>(0));
    REQUIRE(context.item_cnt == 0U);
    REQUIRE(context.count == 0U);
    REQUIRE(context.inited == CIRCLE_QUEUE_FALSE);
    REQUIRE(Interface_CircleQueue_Destroy(&context) == CIRCLE_QUEUE_FALSE);
    REQUIRE(Interface_CircleQueue_IsEmpty(&context) == CIRCLE_QUEUE_TRUE);
}

TEST_CASE("circle queue rejects malformed items without changing state",
          "[circle_queue][validation]")
{
    QueueOwner owner;
    std::uint8_t byte = 0x11U;
    CIRCLE_QUEUE_ITEM invalid = item_for(&byte, 0U);

    REQUIRE(Interface_CircleQueue_Create(&owner.context, 2U) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &invalid) ==
            CIRCLE_QUEUE_FALSE);
    invalid = item_for(static_cast<std::uint8_t *>(0), 1U);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &invalid) ==
            CIRCLE_QUEUE_FALSE);
    REQUIRE(Interface_CircleQueue_Count(&owner.context) == 0U);
    REQUIRE(Interface_CircleQueue_IsEmpty(&owner.context) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Pop(&owner.context, static_cast<PCIRCLE_QUEUE_ITEM>(0)) ==
            CIRCLE_QUEUE_FALSE);
}

TEST_CASE("circle queue preserves FIFO order and borrowed item metadata",
          "[circle_queue][fifo]")
{
    QueueOwner owner;
    std::array<std::uint8_t, 3U> bytes = {{0x10U, 0x20U, 0x30U}};
    const CIRCLE_QUEUE_ITEM expected[] = {
        item_for(&bytes[0], 1U), item_for(&bytes[1], 2U),
        item_for(&bytes[2], 3U)};
    CIRCLE_QUEUE_ITEM actual = {};
    std::size_t index = 0U;

    REQUIRE(Interface_CircleQueue_Create(&owner.context, 3U) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Capacity(&owner.context) == 3U);
    REQUIRE(Interface_CircleQueue_IsEmpty(&owner.context) == CIRCLE_QUEUE_TRUE);

    while (index < 3U) {
        REQUIRE(Interface_CircleQueue_Push(&owner.context, &expected[index]) ==
                CIRCLE_QUEUE_TRUE);
        REQUIRE(Interface_CircleQueue_Count(&owner.context) ==
                static_cast<std::uint32_t>(index + 1U));
        ++index;
    }

    index = 0U;
    while (index < 3U) {
        REQUIRE(Interface_CircleQueue_Pop(&owner.context, &actual) ==
                CIRCLE_QUEUE_TRUE);
        require_item_equal(actual, expected[index]);
        ++index;
    }

    REQUIRE(Interface_CircleQueue_Count(&owner.context) == 0U);
    REQUIRE(Interface_CircleQueue_IsEmpty(&owner.context) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Pop(&owner.context, &actual) == CIRCLE_QUEUE_FALSE);
}

TEST_CASE("circle queue supports wrap-around and explicit empty Pop",
          "[circle_queue][wrap]")
{
    QueueOwner owner;
    std::array<std::uint8_t, 5U> bytes = {{1U, 2U, 3U, 4U, 5U}};
    CIRCLE_QUEUE_ITEM actual = {};
    CIRCLE_QUEUE_ITEM item = item_for(&bytes[0], 1U);

    REQUIRE(Interface_CircleQueue_Create(&owner.context, 3U) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &item) == CIRCLE_QUEUE_TRUE);
    item = item_for(&bytes[1], 2U);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &item) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Pop(&owner.context, &actual) == CIRCLE_QUEUE_TRUE);
    require_item_equal(actual, item_for(&bytes[0], 1U));

    item = item_for(&bytes[2], 3U);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &item) == CIRCLE_QUEUE_TRUE);
    item = item_for(&bytes[3], 4U);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &item) == CIRCLE_QUEUE_TRUE);
    item = item_for(&bytes[4], 5U);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &item) == CIRCLE_QUEUE_TRUE);

    REQUIRE(Interface_CircleQueue_Count(&owner.context) == 3U);
    REQUIRE(Interface_CircleQueue_Pop(&owner.context, &actual) == CIRCLE_QUEUE_TRUE);
    require_item_equal(actual, item_for(&bytes[2], 3U));
    REQUIRE(Interface_CircleQueue_Pop(&owner.context, &actual) == CIRCLE_QUEUE_TRUE);
    require_item_equal(actual, item_for(&bytes[3], 4U));
    REQUIRE(Interface_CircleQueue_Pop(&owner.context, &actual) == CIRCLE_QUEUE_TRUE);
    require_item_equal(actual, item_for(&bytes[4], 5U));
    REQUIRE(Interface_CircleQueue_Pop(&owner.context, static_cast<PCIRCLE_QUEUE_ITEM>(0)) ==
            CIRCLE_QUEUE_FALSE);
}

TEST_CASE("full circle queue overwrites its oldest item",
          "[circle_queue][overwrite]")
{
    QueueOwner owner;
    std::array<std::uint8_t, 4U> bytes = {{0xA0U, 0xB0U, 0xC0U, 0xD0U}};
    CIRCLE_QUEUE_ITEM actual = {};
    CIRCLE_QUEUE_ITEM item = item_for(&bytes[0], 10U);

    REQUIRE(Interface_CircleQueue_Create(&owner.context, 2U) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &item) == CIRCLE_QUEUE_TRUE);
    item = item_for(&bytes[1], 11U);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &item) == CIRCLE_QUEUE_TRUE);
    item = item_for(&bytes[2], 12U);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &item) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Count(&owner.context) == 2U);

    REQUIRE(Interface_CircleQueue_Pop(&owner.context, &actual) == CIRCLE_QUEUE_TRUE);
    require_item_equal(actual, item_for(&bytes[1], 11U));
    item = item_for(&bytes[3], 13U);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &item) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Pop(&owner.context, &actual) == CIRCLE_QUEUE_TRUE);
    require_item_equal(actual, item_for(&bytes[2], 12U));
    REQUIRE(Interface_CircleQueue_Pop(&owner.context, &actual) == CIRCLE_QUEUE_TRUE);
    require_item_equal(actual, item_for(&bytes[3], 13U));
}

TEST_CASE("capacity-one queue always retains the newest item",
          "[circle_queue][overwrite][boundary]")
{
    QueueOwner owner;
    std::array<std::uint8_t, 3U> bytes = {{7U, 8U, 9U}};
    CIRCLE_QUEUE_ITEM actual = {};
    CIRCLE_QUEUE_ITEM item = item_for(&bytes[0], 7U);

    REQUIRE(Interface_CircleQueue_Create(&owner.context, 1U) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &item) == CIRCLE_QUEUE_TRUE);
    item = item_for(&bytes[1], 8U);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &item) == CIRCLE_QUEUE_TRUE);
    item = item_for(&bytes[2], 9U);
    REQUIRE(Interface_CircleQueue_Push(&owner.context, &item) == CIRCLE_QUEUE_TRUE);
    REQUIRE(Interface_CircleQueue_Count(&owner.context) == 1U);
    REQUIRE(Interface_CircleQueue_Pop(&owner.context, &actual) == CIRCLE_QUEUE_TRUE);
    require_item_equal(actual, item_for(&bytes[2], 9U));
    REQUIRE(Interface_CircleQueue_IsEmpty(&owner.context) == CIRCLE_QUEUE_TRUE);
}

TEST_CASE("circle queue matches a reference model across long mixed traffic",
          "[circle_queue][property]")
{
    QueueOwner owner;
    std::array<std::uint8_t, 256U> bytes = {};
    std::deque<CIRCLE_QUEUE_ITEM> expected;
    CIRCLE_QUEUE_ITEM actual = {};
    std::uint32_t state = 0x13579BDFU;
    std::uint32_t iteration = 0U;

    REQUIRE(Interface_CircleQueue_Create(&owner.context, 7U) == CIRCLE_QUEUE_TRUE);

    while (iteration < 20000U) {
        state = state * 1664525U + 1013904223U;
        if ((state & 3U) != 0U || expected.empty()) {
            const std::size_t slot = static_cast<std::size_t>(state & 255U);
            const std::uint32_t length = 1U + ((state >> 8U) & 31U);
            CIRCLE_QUEUE_ITEM item = item_for(&bytes[slot], length);

            REQUIRE(Interface_CircleQueue_Push(&owner.context, &item) ==
                    CIRCLE_QUEUE_TRUE);
            if (expected.size() == 7U) {
                expected.pop_front();
            }
            expected.push_back(item);
        } else {
            REQUIRE(Interface_CircleQueue_Pop(&owner.context, &actual) ==
                    CIRCLE_QUEUE_TRUE);
            require_item_equal(actual, expected.front());
            expected.pop_front();
        }

        REQUIRE(Interface_CircleQueue_Count(&owner.context) ==
                static_cast<std::uint32_t>(expected.size()));
        REQUIRE(Interface_CircleQueue_IsEmpty(&owner.context) ==
                (expected.empty() ? CIRCLE_QUEUE_TRUE : CIRCLE_QUEUE_FALSE));
        ++iteration;
    }

    while (!expected.empty()) {
        REQUIRE(Interface_CircleQueue_Pop(&owner.context, &actual) ==
                CIRCLE_QUEUE_TRUE);
        require_item_equal(actual, expected.front());
        expected.pop_front();
    }
    REQUIRE(Interface_CircleQueue_IsEmpty(&owner.context) == CIRCLE_QUEUE_TRUE);
}
