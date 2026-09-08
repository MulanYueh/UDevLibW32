/*
 * User-mode demo for worker_thread_pool.
 *
 * Build with the pool implementation and its private support libraries:
 *
 *     cl /nologo /W4 /DWORKER_THREAD_POOL_USE_STDINT2 /TC ^
 *         worker_thread_pool_demo.c allocator.c ^
 *         worker_thread_pool.c list.c lock.c /Fe:worker_thread_pool_demo.exe
 *
 * Or with MinGW:
 *
 *     gcc -std=c11 -DWORKER_THREAD_POOL_USE_STDINT2 \
 *         -Wall -Wextra -Wno-unknown-pragmas \
 *         worker_thread_pool_demo.c allocator.c worker_thread_pool.c \
 *         list.c lock.c -o worker_thread_pool_demo.exe
 */

#ifndef WORKER_THREAD_POOL_USE_STDINT2
#define WORKER_THREAD_POOL_USE_STDINT2 1
#endif
#include "worker_thread_pool.h"

#include <stdio.h>

typedef struct DEMO_COUNTER {
    volatile LONG task_count;
    volatile LONG finish_count;
    volatile LONG worker_calls;
    LONG target_finish_count;
    HANDLE complete_event;
} DEMO_COUNTER;

typedef struct DEMO_TASK_CONTEXT {
    worker_thread_pool *pool;
    DEMO_COUNTER *counter;
    HANDLE started_event;
    HANDLE release_event;
    volatile LONG *started_count;
    LONG started_target;
    int wait_for_release;
} DEMO_TASK_CONTEXT;

static const char *demo_status_name(int32_t status)
{
    switch (status) {
    case WORKER_THREAD_POOL_SUCCESS:
        return "SUCCESS";
    case WORKER_THREAD_POOL_INVALID_PARAMETER:
        return "INVALID_PARAMETER";
    case WORKER_THREAD_POOL_NO_MEMORY:
        return "NO_MEMORY";
    case WORKER_THREAD_POOL_QUEUE_FULL:
        return "QUEUE_FULL";
    case WORKER_THREAD_POOL_STOPPED:
        return "STOPPED";
    case WORKER_THREAD_POOL_TIMEOUT:
        return "TIMEOUT";
    default:
        return "UNKNOWN";
    }
}

static int demo_counter_init(DEMO_COUNTER *counter,
                             LONG target_finish_count)
{
    if (counter == NULL || target_finish_count <= 0) {
        return 0;
    }
    counter->task_count = 0;
    counter->finish_count = 0;
    counter->worker_calls = 0;
    counter->target_finish_count = target_finish_count;
    counter->complete_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    return counter->complete_event != NULL;
}

static void demo_counter_destroy(DEMO_COUNTER *counter)
{
    if (counter != NULL && counter->complete_event != NULL) {
        (void)CloseHandle(counter->complete_event);
        counter->complete_event = NULL;
    }
}

static void demo_task(void *context)
{
    DEMO_TASK_CONTEXT *task_context = NULL;
    DEMO_COUNTER *counter = NULL;
    LONG started = 0;

    task_context = (DEMO_TASK_CONTEXT *)context;
    if (task_context == NULL || task_context->counter == NULL) {
        return;
    }
    counter = task_context->counter;

    if (task_context->pool != NULL &&
        worker_thread_pool_is_worker(task_context->pool,
                                      (ULONG_PTR)GetCurrentThreadId())) {
        (void)InterlockedIncrement(&counter->worker_calls);
    }

    if (task_context->wait_for_release != 0 &&
        task_context->started_count != NULL &&
        task_context->started_event != NULL &&
        task_context->release_event != NULL) {
        started = InterlockedIncrement(task_context->started_count);
        if (started >= task_context->started_target) {
            (void)SetEvent(task_context->started_event);
        }
        (void)WaitForSingleObject(task_context->release_event, INFINITE);
    }

    (void)InterlockedIncrement(&counter->task_count);
}

static void demo_finish(void *context)
{
    DEMO_TASK_CONTEXT *task_context = NULL;
    DEMO_COUNTER *counter = NULL;
    LONG finished = 0;

    task_context = (DEMO_TASK_CONTEXT *)context;
    if (task_context == NULL || task_context->counter == NULL) {
        return;
    }
    counter = task_context->counter;
    finished = InterlockedIncrement(&counter->finish_count);
    if (finished >= counter->target_finish_count) {
        (void)SetEvent(counter->complete_event);
    }
}

static int demo_wait(HANDLE event, DWORD timeout_ms, const char *name)
{
    DWORD result = WAIT_FAILED;

    if (event == NULL) {
        printf("wait failed: %s (event is NULL)\n", name);
        return 0;
    }
    result = WaitForSingleObject(event, timeout_ms);
    if (result == WAIT_OBJECT_0) {
        return 1;
    }
    if (result == WAIT_TIMEOUT) {
        printf("wait timeout: %s\n", name);
    } else {
        printf("wait failed: %s error=%lu\n", name,
               (unsigned long)GetLastError());
    }
    return 0;
}

int main(void)
{
    worker_thread_pool *pool = NULL;
    DEMO_COUNTER sync_counter = {0, 0, 0, 0, NULL};
    DEMO_COUNTER timeout_counter = {0, 0, 0, 0, NULL};
    DEMO_COUNTER batch_counter = {0, 0, 0, 0, NULL};
    DEMO_TASK_CONTEXT sync_context = {NULL, NULL, NULL, NULL, NULL, 0, 0};
    DEMO_TASK_CONTEXT timeout_context = {NULL, NULL, NULL, NULL, NULL, 0, 0};
    DEMO_TASK_CONTEXT blocking_context = {NULL, NULL, NULL, NULL, NULL, 0, 0};
    DEMO_TASK_CONTEXT quick_context = {NULL, NULL, NULL, NULL, NULL, 0, 0};
    HANDLE timeout_started_event = NULL;
    HANDLE timeout_release_event = NULL;
    HANDLE batch_started_event = NULL;
    HANDLE batch_release_event = NULL;
    volatile LONG timeout_started_count = 0;
    volatile LONG batch_started_count = 0;
    int32_t status = WORKER_THREAD_POOL_INVALID_PARAMETER;
    int32_t full_status = WORKER_THREAD_POOL_INVALID_PARAMETER;
    int accepted = 0;
    int index = 0;
    int exit_code = 1;

    if (!demo_counter_init(&sync_counter, 1)) {
        puts("sync counter initialization failed");
        goto cleanup;
    }
    if (!demo_counter_init(&timeout_counter, 1)) {
        puts("timeout counter initialization failed");
        goto cleanup;
    }
    if (!demo_counter_init(&batch_counter, 6)) {
        puts("batch counter initialization failed");
        goto cleanup;
    }

    timeout_started_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    timeout_release_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    batch_started_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    batch_release_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (timeout_started_event == NULL || timeout_release_event == NULL ||
        batch_started_event == NULL || batch_release_event == NULL) {
        puts("event initialization failed");
        goto cleanup;
    }

    pool = worker_thread_pool_create(2u, 4u);
    if (pool == NULL) {
        puts("worker_thread_pool_create failed");
        goto cleanup;
    }
    printf("pool created: workers=%lu\n",
           (unsigned long)worker_thread_pool_worker_count(pool));

    sync_context.pool = pool;
    sync_context.counter = &sync_counter;
    sync_context.started_event = NULL;
    sync_context.release_event = NULL;
    sync_context.started_count = NULL;
    sync_context.started_target = 0;
    sync_context.wait_for_release = 0;

    status = worker_thread_pool_submit(pool, demo_task, demo_finish,
                                       &sync_context, 1,
                                       WORKER_THREAD_POOL_WAIT_INFINITE);
    printf("1) synchronous task: %s tasks=%ld finishes=%ld worker_calls=%ld\n",
           demo_status_name(status), (long)sync_counter.task_count,
           (long)sync_counter.finish_count, (long)sync_counter.worker_calls);
    if (status != WORKER_THREAD_POOL_SUCCESS) {
        goto cleanup;
    }

    timeout_context.pool = pool;
    timeout_context.counter = &timeout_counter;
    timeout_context.started_event = timeout_started_event;
    timeout_context.release_event = timeout_release_event;
    timeout_context.started_count = &timeout_started_count;
    timeout_context.started_target = 1;
    timeout_context.wait_for_release = 1;

    status = worker_thread_pool_submit(pool, demo_task, demo_finish,
                                       &timeout_context, 1, 0u);
    printf("2) zero-timeout task: %s (expected TIMEOUT)\n",
           demo_status_name(status));
    if (status != WORKER_THREAD_POOL_TIMEOUT) {
        puts("zero-timeout submission did not return TIMEOUT");
        goto cleanup;
    }
    (void)SetEvent(timeout_release_event);
    if (!demo_wait(timeout_counter.complete_event, 5000u,
                   "timeout task completion")) {
        goto cleanup;
    }

    blocking_context.pool = pool;
    blocking_context.counter = &batch_counter;
    blocking_context.started_event = batch_started_event;
    blocking_context.release_event = batch_release_event;
    blocking_context.started_count = &batch_started_count;
    blocking_context.started_target = 2;
    blocking_context.wait_for_release = 1;

    quick_context.pool = pool;
    quick_context.counter = &batch_counter;
    quick_context.started_event = NULL;
    quick_context.release_event = NULL;
    quick_context.started_count = NULL;
    quick_context.started_target = 0;
    quick_context.wait_for_release = 0;

    status = worker_thread_pool_submit(pool, demo_task, demo_finish,
                                       &blocking_context, 0, 0u);
    if (status != WORKER_THREAD_POOL_SUCCESS) {
        printf("blocking task 1: %s\n", demo_status_name(status));
        goto cleanup;
    }
    status = worker_thread_pool_submit(pool, demo_task, demo_finish,
                                       &blocking_context, 0, 0u);
    if (status != WORKER_THREAD_POOL_SUCCESS) {
        printf("blocking task 2: %s\n", demo_status_name(status));
        goto cleanup;
    }
    if (!demo_wait(batch_started_event, 5000u, "two blocking workers")) {
        goto cleanup;
    }

    accepted = 0;
    for (index = 0; index < 4; ++index) {
        status = worker_thread_pool_submit(pool, demo_task, demo_finish,
                                           &quick_context, 0, 0u);
        if (status == WORKER_THREAD_POOL_SUCCESS) {
            ++accepted;
        }
    }
    full_status = worker_thread_pool_submit(pool, demo_task, demo_finish,
                                            &quick_context, 0, 0u);
    printf("3) queue pressure: accepted=%d full_submit=%s\n", accepted,
           demo_status_name(full_status));
    if (accepted != 4 || full_status != WORKER_THREAD_POOL_QUEUE_FULL) {
        goto cleanup;
    }

    (void)SetEvent(batch_release_event);
    if (!demo_wait(batch_counter.complete_event, 5000u,
                   "queued batch completion")) {
        goto cleanup;
    }
    printf("   batch complete: tasks=%ld finishes=%ld worker_calls=%ld\n",
           (long)batch_counter.task_count, (long)batch_counter.finish_count,
           (long)batch_counter.worker_calls);

    status = worker_thread_pool_stop(pool);
    printf("4) stop: %s workers_after_stop=%lu\n", demo_status_name(status),
           (unsigned long)worker_thread_pool_worker_count(pool));
    if (status != WORKER_THREAD_POOL_SUCCESS) {
        goto cleanup;
    }

    status = worker_thread_pool_submit(pool, demo_task, demo_finish,
                                       &sync_context, 0, 0u);
    printf("5) submit after stop: %s\n", demo_status_name(status));
    if (status != WORKER_THREAD_POOL_STOPPED) {
        goto cleanup;
    }

    exit_code = 0;

cleanup:
    /* Always release blocked demo tasks before joining the pool. */
    if (timeout_release_event != NULL) {
        (void)SetEvent(timeout_release_event);
    }
    if (batch_release_event != NULL) {
        (void)SetEvent(batch_release_event);
    }
    if (pool != NULL) {
        worker_thread_pool_destroy(pool);
    }
    if (timeout_started_event != NULL) {
        (void)CloseHandle(timeout_started_event);
    }
    if (timeout_release_event != NULL) {
        (void)CloseHandle(timeout_release_event);
    }
    if (batch_started_event != NULL) {
        (void)CloseHandle(batch_started_event);
    }
    if (batch_release_event != NULL) {
        (void)CloseHandle(batch_release_event);
    }
    demo_counter_destroy(&sync_counter);
    demo_counter_destroy(&timeout_counter);
    demo_counter_destroy(&batch_counter);
    return exit_code;
}
