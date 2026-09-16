/*
 * Catch2 regression tests for the user-mode worker thread pool.
 *
 * Build the C implementation as C, then link it with this C++ translation
 * unit and the repository-local Catch2 v2 header:
 *
 *   gcc -std=c11 -Wall -Wextra -Wconversion -Wshadow -Werror -c \
 *       worker_thread_pool.c allocator.c list.c lock.c
 *   g++ -std=c++17 -Wall -Wextra -Wconversion -Wshadow -Werror -I. \
 *       worker_thread_pool_unit_test.cpp worker_thread_pool.o allocator.o \
 *       list.o lock.o -o worker_thread_pool_unit_test.exe
 */

#define CATCH_CONFIG_MAIN
#include "catch2/catch.hpp"

#include "worker_thread_pool.h"

#include <thread>
#include <vector>

namespace
{

struct EventHandle
{
    HANDLE handle;

    EventHandle() : handle(nullptr)
    {
        handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~EventHandle()
    {
        if (handle != nullptr) {
            (void)CloseHandle(handle);
            handle = nullptr;
        }
    }

    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;

    bool valid() const
    {
        return handle != nullptr;
    }
};

struct PoolHandle
{
    worker_thread_pool *pool;

    PoolHandle(uint32_t worker_count, uint32_t queue_limit) : pool(nullptr)
    {
        pool = worker_thread_pool_create(worker_count, queue_limit);
    }

    ~PoolHandle()
    {
        if (pool != nullptr) {
            worker_thread_pool_destroy(pool);
            pool = nullptr;
        }
    }

    PoolHandle(const PoolHandle&) = delete;
    PoolHandle& operator=(const PoolHandle&) = delete;
};

struct TaskState
{
    worker_thread_pool *pool;
    HANDLE started_event;
    HANDLE release_event;
    HANDLE complete_event;
    volatile LONG task_count;
    volatile LONG finish_count;
    volatile LONG worker_calls;
    volatile LONG started_count;
    LONG started_target;
    LONG expected_finish_count;
    int wait_for_release;
    int call_stop_from_worker;
    int32_t worker_stop_status;

    TaskState()
        : pool(nullptr),
          started_event(nullptr),
          release_event(nullptr),
          complete_event(nullptr),
          task_count(0),
          finish_count(0),
          worker_calls(0),
          started_count(0),
          started_target(0),
          expected_finish_count(0),
          wait_for_release(0),
          call_stop_from_worker(0),
          worker_stop_status(WORKER_THREAD_POOL_INVALID_PARAMETER)
    {
    }
};

bool wait_for_event(HANDLE handle, DWORD timeout_ms)
{
    DWORD result = WAIT_FAILED;

    if (handle == nullptr) {
        return false;
    }
    result = WaitForSingleObject(handle, timeout_ms);
    return result == WAIT_OBJECT_0;
}

void allow_worker_bookkeeping()
{
    /* Completion is signaled before the worker releases its job reference. */
    Sleep(10u);
}

void counting_task(void *context)
{
    TaskState *state = static_cast<TaskState *>(context);
    ULONG_PTR current_thread_id = 0u;
    LONG started = 0;

    if (state == nullptr) {
        return;
    }

    current_thread_id = static_cast<ULONG_PTR>(GetCurrentThreadId());
    if (state->pool != nullptr &&
        worker_thread_pool_is_worker(state->pool, current_thread_id)) {
        (void)InterlockedIncrement(&state->worker_calls);
    }

    if (state->call_stop_from_worker != 0 && state->pool != nullptr) {
        state->worker_stop_status = worker_thread_pool_stop(state->pool);
    }

    if (state->wait_for_release != 0) {
        started = InterlockedIncrement(&state->started_count);
        if (state->started_event != nullptr && started >= state->started_target) {
            (void)SetEvent(state->started_event);
        }
        if (state->release_event != nullptr) {
            (void)WaitForSingleObject(state->release_event, INFINITE);
        }
    }

    (void)InterlockedIncrement(&state->task_count);
}

void counting_finish(void *context)
{
    TaskState *state = static_cast<TaskState *>(context);
    LONG finished = 0;

    if (state == nullptr) {
        return;
    }
    finished = InterlockedIncrement(&state->finish_count);
    if (state->complete_event != nullptr &&
        finished >= state->expected_finish_count) {
        (void)SetEvent(state->complete_event);
    }
}

int32_t submit_async(worker_thread_pool *pool, TaskState *state)
{
    if (pool == nullptr || state == nullptr) {
        return WORKER_THREAD_POOL_INVALID_PARAMETER;
    }
    return worker_thread_pool_submit(pool, counting_task, counting_finish,
                                     state, 0, 0u);
}

} // namespace

TEST_CASE("worker thread pool rejects invalid parameters", "[worker_thread_pool][validation]")
{
    worker_thread_pool *pool = nullptr;
    int32_t status = WORKER_THREAD_POOL_SUCCESS;

    pool = worker_thread_pool_create(0u, 1u);
    CHECK(pool == nullptr);
    pool = worker_thread_pool_create(1u, 0u);
    CHECK(pool == nullptr);
    CHECK(worker_thread_pool_worker_count(nullptr) == 0u);
    CHECK(worker_thread_pool_is_worker(nullptr, 0u) == 0);
    status = worker_thread_pool_submit(nullptr, counting_task, nullptr, nullptr, 0,
                                       0u);
    CHECK(status == WORKER_THREAD_POOL_INVALID_PARAMETER);
    status = worker_thread_pool_submit(nullptr, nullptr, nullptr, nullptr, 0, 0u);
    CHECK(status == WORKER_THREAD_POOL_INVALID_PARAMETER);
    status = worker_thread_pool_stop(nullptr);
    CHECK(status == WORKER_THREAD_POOL_INVALID_PARAMETER);
    worker_thread_pool_destroy(nullptr);
}

TEST_CASE("synchronous submission runs task and finish once",
          "[worker_thread_pool][synchronous]")
{
    EventHandle complete_event = EventHandle();
    PoolHandle pool = PoolHandle(2u, 8u);
    TaskState state = TaskState();
    int32_t status = WORKER_THREAD_POOL_INVALID_PARAMETER;

    REQUIRE(complete_event.valid());
    REQUIRE(pool.pool != nullptr);
    state.pool = pool.pool;
    state.complete_event = complete_event.handle;
    state.expected_finish_count = 1;

    status = worker_thread_pool_submit(
        pool.pool, counting_task, counting_finish, &state, 1,
        WORKER_THREAD_POOL_WAIT_INFINITE);
    REQUIRE(status == WORKER_THREAD_POOL_SUCCESS);
    CHECK(state.task_count == 1);
    CHECK(state.finish_count == 1);
    CHECK(state.worker_calls == 1);
    CHECK(wait_for_event(complete_event.handle, 5000u));
    CHECK(worker_thread_pool_worker_count(pool.pool) == 2u);
    CHECK(worker_thread_pool_is_worker(
              pool.pool, static_cast<ULONG_PTR>(GetCurrentThreadId())) == 0);
    allow_worker_bookkeeping();
    CHECK(worker_thread_pool_stop(pool.pool) == WORKER_THREAD_POOL_SUCCESS);
}

TEST_CASE("asynchronous submissions execute every accepted task",
          "[worker_thread_pool][asynchronous]")
{
    const int task_total = 2000;
    EventHandle complete_event = EventHandle();
    PoolHandle pool = PoolHandle(2u, 2048u);
    TaskState state = TaskState();
    int accepted = 0;
    int index = 0;
    int32_t status = WORKER_THREAD_POOL_INVALID_PARAMETER;

    REQUIRE(complete_event.valid());
    REQUIRE(pool.pool != nullptr);
    state.pool = pool.pool;
    state.complete_event = complete_event.handle;
    state.expected_finish_count = task_total;

    for (index = 0; index < task_total; ++index) {
        status = submit_async(pool.pool, &state);
        if (status == WORKER_THREAD_POOL_SUCCESS) {
            ++accepted;
        }
    }
    REQUIRE(accepted == task_total);
    REQUIRE(wait_for_event(complete_event.handle, 5000u));
    CHECK(state.task_count == task_total);
    CHECK(state.finish_count == task_total);
    CHECK(state.worker_calls == task_total);
    allow_worker_bookkeeping();
    CHECK(worker_thread_pool_stop(pool.pool) == WORKER_THREAD_POOL_SUCCESS);
}

TEST_CASE("repeated create and destroy cycles remain usable",
          "[worker_thread_pool][stress]")
{
    const int cycle_total = 25;
    int cycle = 0;
    int32_t status = WORKER_THREAD_POOL_INVALID_PARAMETER;

    for (cycle = 0; cycle < cycle_total; ++cycle) {
        EventHandle complete_event = EventHandle();
        PoolHandle pool = PoolHandle(2u, 8u);
        TaskState state = TaskState();

        REQUIRE(complete_event.valid());
        REQUIRE(pool.pool != nullptr);
        state.pool = pool.pool;
        state.complete_event = complete_event.handle;
        state.expected_finish_count = 1;
        status = worker_thread_pool_submit(
            pool.pool, counting_task, counting_finish, &state, 1,
            WORKER_THREAD_POOL_WAIT_INFINITE);
        REQUIRE(status == WORKER_THREAD_POOL_SUCCESS);
        CHECK(state.task_count == 1);
        CHECK(state.finish_count == 1);
        allow_worker_bookkeeping();
        CHECK(worker_thread_pool_stop(pool.pool) ==
              WORKER_THREAD_POOL_SUCCESS);
    }
    CHECK(cycle == cycle_total);
}

TEST_CASE("synchronous timeout does not cancel the task",
          "[worker_thread_pool][timeout]")
{
    EventHandle started_event = EventHandle();
    EventHandle release_event = EventHandle();
    EventHandle complete_event = EventHandle();
    PoolHandle pool = PoolHandle(1u, 4u);
    TaskState state = TaskState();
    int32_t status = WORKER_THREAD_POOL_INVALID_PARAMETER;

    REQUIRE(started_event.valid());
    REQUIRE(release_event.valid());
    REQUIRE(complete_event.valid());
    REQUIRE(pool.pool != nullptr);
    state.pool = pool.pool;
    state.started_event = started_event.handle;
    state.release_event = release_event.handle;
    state.complete_event = complete_event.handle;
    state.started_target = 1;
    state.expected_finish_count = 1;
    state.wait_for_release = 1;

    status = worker_thread_pool_submit(pool.pool, counting_task, counting_finish,
                                       &state, 1, 0u);
    REQUIRE(status == WORKER_THREAD_POOL_TIMEOUT);
    CHECK(wait_for_event(started_event.handle, 5000u));
    (void)SetEvent(release_event.handle);
    REQUIRE(wait_for_event(complete_event.handle, 5000u));
    CHECK(state.task_count == 1);
    CHECK(state.finish_count == 1);
    allow_worker_bookkeeping();
    CHECK(worker_thread_pool_stop(pool.pool) == WORKER_THREAD_POOL_SUCCESS);
}

TEST_CASE("queue limit is enforced while workers are busy",
          "[worker_thread_pool][queue]")
{
    EventHandle started_event = EventHandle();
    EventHandle release_event = EventHandle();
    EventHandle blocked_complete_event = EventHandle();
    EventHandle quick_complete_event = EventHandle();
    PoolHandle pool = PoolHandle(2u, 4u);
    TaskState blocked = TaskState();
    TaskState quick = TaskState();
    int32_t status = WORKER_THREAD_POOL_INVALID_PARAMETER;
    int32_t full_status = WORKER_THREAD_POOL_INVALID_PARAMETER;
    int index = 0;

    REQUIRE(started_event.valid());
    REQUIRE(release_event.valid());
    REQUIRE(blocked_complete_event.valid());
    REQUIRE(quick_complete_event.valid());
    REQUIRE(pool.pool != nullptr);

    blocked.pool = pool.pool;
    blocked.started_event = started_event.handle;
    blocked.release_event = release_event.handle;
    blocked.complete_event = blocked_complete_event.handle;
    blocked.started_target = 2;
    blocked.expected_finish_count = 2;
    blocked.wait_for_release = 1;

    quick.pool = pool.pool;
    quick.complete_event = quick_complete_event.handle;
    quick.expected_finish_count = 4;

    status = submit_async(pool.pool, &blocked);
    REQUIRE(status == WORKER_THREAD_POOL_SUCCESS);
    status = submit_async(pool.pool, &blocked);
    REQUIRE(status == WORKER_THREAD_POOL_SUCCESS);
    REQUIRE(wait_for_event(started_event.handle, 5000u));

    for (index = 0; index < 4; ++index) {
        status = submit_async(pool.pool, &quick);
        REQUIRE(status == WORKER_THREAD_POOL_SUCCESS);
    }
    full_status = submit_async(pool.pool, &quick);
    CHECK(full_status == WORKER_THREAD_POOL_QUEUE_FULL);

    (void)SetEvent(release_event.handle);
    REQUIRE(wait_for_event(blocked_complete_event.handle, 5000u));
    REQUIRE(wait_for_event(quick_complete_event.handle, 5000u));
    CHECK(blocked.task_count == 2);
    CHECK(blocked.finish_count == 2);
    CHECK(quick.task_count == 4);
    CHECK(quick.finish_count == 4);
    allow_worker_bookkeeping();
    CHECK(worker_thread_pool_stop(pool.pool) == WORKER_THREAD_POOL_SUCCESS);
}

TEST_CASE("stop drains accepted work and rejects later submissions",
          "[worker_thread_pool][stop]")
{
    EventHandle started_event = EventHandle();
    EventHandle release_event = EventHandle();
    EventHandle blocked_complete_event = EventHandle();
    EventHandle quick_complete_event = EventHandle();
    PoolHandle pool = PoolHandle(2u, 8u);
    TaskState blocked = TaskState();
    TaskState quick = TaskState();
    std::thread stopper = std::thread();
    int32_t stop_status = WORKER_THREAD_POOL_INVALID_PARAMETER;
    int32_t submit_status = WORKER_THREAD_POOL_INVALID_PARAMETER;
    int32_t second_stop_status = WORKER_THREAD_POOL_INVALID_PARAMETER;
    int index = 0;

    REQUIRE(started_event.valid());
    REQUIRE(release_event.valid());
    REQUIRE(blocked_complete_event.valid());
    REQUIRE(quick_complete_event.valid());
    REQUIRE(pool.pool != nullptr);

    blocked.pool = pool.pool;
    blocked.started_event = started_event.handle;
    blocked.release_event = release_event.handle;
    blocked.complete_event = blocked_complete_event.handle;
    blocked.started_target = 2;
    blocked.expected_finish_count = 2;
    blocked.wait_for_release = 1;

    quick.pool = pool.pool;
    quick.complete_event = quick_complete_event.handle;
    quick.expected_finish_count = 3;

    for (index = 0; index < 2; ++index) {
        REQUIRE(submit_async(pool.pool, &blocked) ==
                WORKER_THREAD_POOL_SUCCESS);
    }
    REQUIRE(wait_for_event(started_event.handle, 5000u));
    for (index = 0; index < 3; ++index) {
        REQUIRE(submit_async(pool.pool, &quick) ==
                WORKER_THREAD_POOL_SUCCESS);
    }

    stopper = std::thread([&pool, &stop_status]() {
        stop_status = worker_thread_pool_stop(pool.pool);
    });
    (void)SetEvent(release_event.handle);
    stopper.join();

    CHECK(stop_status == WORKER_THREAD_POOL_SUCCESS);
    CHECK(wait_for_event(blocked_complete_event.handle, 5000u));
    CHECK(wait_for_event(quick_complete_event.handle, 5000u));
    CHECK(blocked.finish_count == 2);
    CHECK(quick.finish_count == 3);
    CHECK(worker_thread_pool_worker_count(pool.pool) == 0u);

    second_stop_status = worker_thread_pool_stop(pool.pool);
    CHECK(second_stop_status == WORKER_THREAD_POOL_SUCCESS);
    submit_status = submit_async(pool.pool, &quick);
    CHECK(submit_status == WORKER_THREAD_POOL_STOPPED);
}

TEST_CASE("worker cannot stop its own pool",
          "[worker_thread_pool][lifecycle]")
{
    EventHandle complete_event = EventHandle();
    PoolHandle pool = PoolHandle(1u, 4u);
    TaskState state = TaskState();
    int32_t status = WORKER_THREAD_POOL_INVALID_PARAMETER;

    REQUIRE(complete_event.valid());
    REQUIRE(pool.pool != nullptr);
    state.pool = pool.pool;
    state.complete_event = complete_event.handle;
    state.expected_finish_count = 1;
    state.call_stop_from_worker = 1;

    status = worker_thread_pool_submit(
        pool.pool, counting_task, counting_finish, &state, 1,
        WORKER_THREAD_POOL_WAIT_INFINITE);
    REQUIRE(status == WORKER_THREAD_POOL_SUCCESS);
    CHECK(state.worker_stop_status == WORKER_THREAD_POOL_INVALID_PARAMETER);
    CHECK(state.finish_count == 1);
    allow_worker_bookkeeping();
    CHECK(worker_thread_pool_stop(pool.pool) == WORKER_THREAD_POOL_SUCCESS);
}

TEST_CASE("concurrent stop callers all observe success",
          "[worker_thread_pool][concurrency]")
{
    const int stopper_count = 8;
    const int task_total = 16;
    EventHandle complete_event = EventHandle();
    PoolHandle pool = PoolHandle(4u, 32u);
    TaskState state = TaskState();
    std::vector<std::thread> stoppers = {};
    volatile LONG failures = 0;
    int accepted = 0;
    int index = 0;
    int32_t status = WORKER_THREAD_POOL_INVALID_PARAMETER;

    REQUIRE(complete_event.valid());
    REQUIRE(pool.pool != nullptr);
    state.pool = pool.pool;
    state.complete_event = complete_event.handle;
    state.expected_finish_count = task_total;

    for (index = 0; index < task_total; ++index) {
        status = submit_async(pool.pool, &state);
        if (status == WORKER_THREAD_POOL_SUCCESS) {
            ++accepted;
        }
    }
    REQUIRE(accepted == task_total);
    REQUIRE(wait_for_event(complete_event.handle, 5000u));
    allow_worker_bookkeeping();

    stoppers.reserve(static_cast<std::size_t>(stopper_count));
    for (index = 0; index < stopper_count; ++index) {
        stoppers.emplace_back([&pool, &failures]() {
            int32_t stop_status = WORKER_THREAD_POOL_INVALID_PARAMETER;

            stop_status = worker_thread_pool_stop(pool.pool);
            if (stop_status != WORKER_THREAD_POOL_SUCCESS) {
                (void)InterlockedIncrement(&failures);
            }
        });
    }
    for (index = 0; index < stopper_count; ++index) {
        if (stoppers[static_cast<std::size_t>(index)].joinable()) {
            stoppers[static_cast<std::size_t>(index)].join();
        }
    }

    CHECK(failures == 0);
    CHECK(worker_thread_pool_worker_count(pool.pool) == 0u);
}
