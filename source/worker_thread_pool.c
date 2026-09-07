#include "worker_thread_pool.h"

#include "def.h"
#include "allocator.h"
#include "list.h"
#include "lock.h"

#ifndef WORKER_THREAD_POOL_HAS_PLATFORM_HEADERS
#  define WORKER_THREAD_POOL_HAS_PLATFORM_HEADERS 0
#  include <stddef.h>
#  ifndef LPC_ALPC_PORT_COMMON_TYPES_DEFINED
#    define LPC_ALPC_PORT_COMMON_TYPES_DEFINED 1
/*
 * Keep this fallback block identical to the one in lpc_port.h/alpc_port.h.
 * The worker header is often included first by C++ applications; defining
 * only the handful of worker scalar types here used to make the subsequent
 * LPC/ALPC include silently omit ANSI_STRING and OBJECT_ATTRIBUTES.
 */
typedef uint8_t UCHAR;
typedef uint8_t BOOLEAN;
typedef uint16_t USHORT;
typedef unsigned long ULONG;
#if defined(_WIN64) || defined(_M_AMD64) || defined(__x86_64__) || \
    defined(_M_ARM64) || defined(__aarch64__)
typedef unsigned long long ULONG_PTR;
#else
typedef unsigned long ULONG_PTR;
#endif
typedef int32_t LONG;
typedef int64_t LONGLONG;
typedef uint32_t ACCESS_MASK;
typedef void* PVOID;
typedef void* HANDLE;
typedef HANDLE* PHANDLE;
typedef char* PCHAR;
typedef const char* PCSZ;
typedef ULONG* PULONG;
typedef size_t SIZE_T;
typedef SIZE_T* PSIZE_T;
typedef struct _LPC_LARGE_INTEGER { int64_t QuadPart; } LARGE_INTEGER;
typedef LARGE_INTEGER* PLARGE_INTEGER;
typedef struct _LPC_ANSI_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
} ANSI_STRING, * PANSI_STRING;
typedef struct _LPC_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    wchar_t* Buffer;
} UNICODE_STRING, * PUNICODE_STRING;
typedef const UNICODE_STRING* PCUNICODE_STRING;
typedef struct _LPC_OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES, * POBJECT_ATTRIBUTES;
typedef struct _LPC_SECURITY_QUALITY_OF_SERVICE {
    ULONG Length;
    ULONG ImpersonationLevel;
    BOOLEAN ContextTrackingMode;
    BOOLEAN EffectiveOnly;
} SECURITY_QUALITY_OF_SERVICE, * PSECURITY_QUALITY_OF_SERVICE;
typedef PVOID PSECURITY_DESCRIPTOR;
typedef LONG NTSTATUS;
#  endif
#  ifndef TRUE
#    define TRUE 1
#  endif
#  ifndef FALSE
#    define FALSE 0
#  endif
#  ifndef STATUS_SUCCESS
#    define STATUS_SUCCESS ((NTSTATUS)(LONG)0UL)
#  endif
#  ifndef STATUS_TIMEOUT
#    define STATUS_TIMEOUT ((NTSTATUS)(LONG)0x00000102UL)
#  endif
#  ifndef STATUS_UNSUCCESSFUL
#    define STATUS_UNSUCCESSFUL ((NTSTATUS)(LONG)0xC0000001UL)
#  endif
#  ifndef NT_SUCCESS
#    define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#  endif
#  if defined(_KERNEL_MODE)
typedef struct _WTP_KEVENT { LONG state; } KEVENT;
typedef void VOID;
typedef enum _WTP_WAIT_REASON { Executive = 0 } KWAIT_REASON;
#    ifndef KernelMode
#      define KernelMode 0
#    endif
#    ifndef NotificationEvent
#      define NotificationEvent 0
#    endif
#    ifndef IO_NO_INCREMENT
#      define IO_NO_INCREMENT 0
#    endif
#    ifndef THREAD_ALL_ACCESS
#      define THREAD_ALL_ACCESS 0UL
#    endif
extern void KeInitializeEvent(KEVENT* event, int type, BOOLEAN state);
extern LONG KeSetEvent(KEVENT* event, int increment, BOOLEAN wait);
extern LONG KeResetEvent(KEVENT* event);
extern NTSTATUS KeWaitForSingleObject(void* object, KWAIT_REASON reason,
    int mode, BOOLEAN alertable,
    PLARGE_INTEGER timeout);
extern ULONG_PTR PsGetCurrentThreadId(void);
extern NTSTATUS PsCreateSystemThread(HANDLE* threadHandle, ULONG desiredAccess,
    PVOID objectAttributes, HANDLE process,
    PVOID clientId, VOID(*startRoutine)(PVOID),
    PVOID startContext);
extern VOID PsTerminateSystemThread(NTSTATUS status);
extern NTSTATUS ZwWaitForSingleObject(HANDLE handle, BOOLEAN alertable,
    PLARGE_INTEGER timeout);
extern NTSTATUS ZwClose(HANDLE handle);
#    ifndef RtlZeroMemory
#      define RtlZeroMemory(destination, length) \
        wtp_fallback_zero_memory((destination), (length))
static inline void wtp_fallback_zero_memory(void* destination, size_t length)
{
    unsigned char* bytes = (unsigned char*)destination;
    while (bytes && length != 0u) {
        *bytes++ = 0u;
        --length;
    }
}
#    endif
#  endif
#endif

enum {
    WTP_JOB_INITIAL = 0,
    WTP_JOB_QUEUED,
    WTP_JOB_RUNNING,
    WTP_JOB_DONE
};

typedef struct wtp_event {
#if defined(_KERNEL_MODE)
    KEVENT native;
#else
    HANDLE native;
#endif
} wtp_event;

typedef struct wtp_job {
    LIST_ELEM link;
    worker_thread_pool_task_fn task;
    worker_thread_pool_task_fn finish;
    void *context;
    wtp_event done;
    volatile uint32_t state;
    volatile uint32_t finish_called;
    volatile uint32_t references;
} wtp_job;

typedef struct wtp_worker {
    worker_thread_pool *pool;
    volatile uint32_t started;
    ULONG_PTR thread_id;
    HANDLE thread_handle;
} wtp_worker;

struct worker_thread_pool {
    LOCK lock;
    LIST jobs;
    wtp_event available;
    wtp_event stopped_event;
    wtp_worker *workers;
    uint32_t worker_limit;
    uint32_t queue_limit;
    volatile uint32_t worker_count;
    volatile uint32_t stopping;
    volatile uint32_t stop_started;
    volatile uint32_t stopped;
};

static void *wtp_alloc(size_t size)
{
#if defined(_KERNEL_MODE)
    return Allocator_Malloc((BOOLEAN)1, size, WTP_POOL_TAG);
#else
    return Allocator_Malloc(size);
#endif
}

static void wtp_free(void *memory)
{
#if defined(_KERNEL_MODE)
    Allocator_Free(memory, WTP_POOL_TAG);
#else
    Allocator_Free(memory);
#endif
}

static void wtp_zero(void *memory, size_t size)
{
#if defined(_KERNEL_MODE)
    RtlZeroMemory(memory, size);
#else
    unsigned char *bytes = (unsigned char *)memory;
    while (size != 0u) {
        *bytes++ = 0u;
        --size;
    }
#endif
}

static uint32_t wtp_atomic_load(const volatile uint32_t *value)
{
#if defined(_MSC_VER)
    return (uint32_t)InterlockedCompareExchange((volatile LONG *)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

static void wtp_atomic_store(volatile uint32_t *value, uint32_t new_value)
{
#if defined(_MSC_VER)
    (void)InterlockedExchange((volatile LONG *)value, (LONG)new_value);
#else
    __atomic_store_n(value, new_value, __ATOMIC_RELEASE);
#endif
}

static uint32_t wtp_atomic_compare_exchange(volatile uint32_t *value,
                                            uint32_t expected,
                                            uint32_t new_value)
{
#if defined(_MSC_VER)
    return (uint32_t)InterlockedCompareExchange((volatile LONG *)value,
                                                 (LONG)new_value,
                                                 (LONG)expected);
#else
    uint32_t expected_value = expected;

    (void)__atomic_compare_exchange_n(value, &expected_value, new_value, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expected_value;
#endif
}

static ULONG_PTR wtp_atomic_thread_id_load(
    const volatile ULONG_PTR *value)
{
#if defined(_MSC_VER)
    return (ULONG_PTR)InterlockedCompareExchangePointer(
        (PVOID volatile *)value, NULL, NULL);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

static void wtp_atomic_thread_id_store(volatile ULONG_PTR *value,
                                       ULONG_PTR new_value)
{
#if defined(_MSC_VER)
    (void)InterlockedExchangePointer((PVOID volatile *)value,
                                      (PVOID)new_value);
#else
    __atomic_store_n(value, new_value, __ATOMIC_RELEASE);
#endif
}

static int wtp_event_init(wtp_event *event, int signaled)
{
#if defined(_KERNEL_MODE)
    KeInitializeEvent(&event->native, NotificationEvent,
                      signaled ? TRUE : FALSE);
    return 1;
#else
    event->native = CreateEventW(NULL, TRUE, signaled ? TRUE : FALSE, NULL);
    return event->native != NULL;
#endif
}

static void wtp_event_destroy(wtp_event *event)
{
#if defined(_KERNEL_MODE)
    (void)event;
#else
    if (event->native != NULL) {
        (void)CloseHandle(event->native);
        event->native = NULL;
    }
#endif
}

static void wtp_event_set(wtp_event *event)
{
#if defined(_KERNEL_MODE)
    (void)KeSetEvent(&event->native, IO_NO_INCREMENT, FALSE);
#else
    (void)SetEvent(event->native);
#endif
}

static void wtp_event_reset(wtp_event *event)
{
#if defined(_KERNEL_MODE)
    (void)KeResetEvent(&event->native);
#else
    (void)ResetEvent(event->native);
#endif
}

/* Returns one when signaled, zero on timeout, and minus one on failure. */
static int wtp_event_wait(wtp_event *event, uint32_t timeout_ms)
{
#if defined(_KERNEL_MODE)
    LARGE_INTEGER interval = {0};
    PLARGE_INTEGER timeout = NULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    if (timeout_ms != WORKER_THREAD_POOL_WAIT_INFINITE) {
        interval.QuadPart = -(LONGLONG)timeout_ms * 10000;
        timeout = &interval;
    }
    status = KeWaitForSingleObject(&event->native, Executive, KernelMode,
                                   FALSE, timeout);
    if (status == STATUS_TIMEOUT) {
        return 0;
    }
    return NT_SUCCESS(status) ? 1 : -1;
#else
    DWORD result = WAIT_FAILED;

    result = WaitForSingleObject(
        event->native,
        timeout_ms == WORKER_THREAD_POOL_WAIT_INFINITE
            ? INFINITE
            : (DWORD)timeout_ms);

    if (result == WAIT_TIMEOUT) {
        return 0;
    }
    return result == WAIT_OBJECT_0 ? 1 : -1;
#endif
}

static ULONG_PTR wtp_current_thread_id(void)
{
#if defined(_KERNEL_MODE)
    return (ULONG_PTR)PsGetCurrentThreadId();
#else
    return (ULONG_PTR)GetCurrentThreadId();
#endif
}

static wtp_job *wtp_job_create(worker_thread_pool_task_fn task,
                               worker_thread_pool_task_fn finish,
                               void *context,
                               uint32_t references)
{
    wtp_job *job = NULL;

    job = (wtp_job *)wtp_alloc(sizeof(*job));

    if (job == NULL) {
        return NULL;
    }
    wtp_zero(job, sizeof(*job));
    if (!wtp_event_init(&job->done, 0)) {
        wtp_free(job);
        return NULL;
    }
    job->task = task;
    job->finish = finish;
    job->context = context;
    job->references = references;
    return job;
}

static void wtp_job_destroy(wtp_job *job)
{
    wtp_event_destroy(&job->done);
    wtp_free(job);
}

static void wtp_job_release(wtp_job *job)
{
    uint32_t references = 0u;

    if (job == NULL) {
        return;
    }
    references = wtp_atomic_load(&job->references);

    while (references != 0u) {
        uint32_t old_value = 0u;

        old_value = wtp_atomic_compare_exchange(
            &job->references, references, references - 1u);
        if (old_value == references) {
            if (references == 1u) {
                wtp_job_destroy(job);
            }
            return;
        }
        references = old_value;
    }
}

static void wtp_job_finish(wtp_job *job)
{
    if (wtp_atomic_compare_exchange(&job->finish_called, 0u, 1u) == 0u &&
        job->finish != NULL) {
        job->finish(job->context);
    }
}

static void wtp_job_run(wtp_job *job)
{
    if (wtp_atomic_compare_exchange(&job->state, WTP_JOB_QUEUED,
                                    WTP_JOB_RUNNING) != WTP_JOB_QUEUED) {
        return;
    }

    job->task(job->context);
    wtp_job_finish(job);
    wtp_atomic_store(&job->state, WTP_JOB_DONE);
    wtp_event_set(&job->done);
}

static int32_t wtp_job_wait(wtp_job *job, uint32_t timeout_ms)
{
    int result = wtp_event_wait(&job->done, timeout_ms);

    if (result > 0) {
        return WORKER_THREAD_POOL_SUCCESS;
    }
    return result == 0
               ? WORKER_THREAD_POOL_TIMEOUT
               : WORKER_THREAD_POOL_STOPPED;
}

static int32_t wtp_queue_put(worker_thread_pool *pool, wtp_job *job)
{
    int32_t status = WORKER_THREAD_POOL_SUCCESS;

    Lock_Exclusive(&pool->lock);
    if (wtp_atomic_load(&pool->stopping) != 0u) {
        status = WORKER_THREAD_POOL_STOPPED;
    } else if (pool->jobs.count >= pool->queue_limit) {
        status = WORKER_THREAD_POOL_QUEUE_FULL;
    } else {
        wtp_atomic_store(&job->state, WTP_JOB_QUEUED);
        if (!List_Insert_After(&pool->jobs, NULL, &job->link)) {
            wtp_atomic_store(&job->state, WTP_JOB_INITIAL);
            status = WORKER_THREAD_POOL_NO_MEMORY;
        } else {
            wtp_event_set(&pool->available);
        }
    }
    Lock_Unlock(&pool->lock);
    return status;
}

static wtp_job *wtp_queue_take(worker_thread_pool *pool)
{
    wtp_job *job = NULL;

    if (pool == NULL) {
        return NULL;
    }

    Lock_Exclusive(&pool->lock);
    job = (wtp_job *)List_Head(&pool->jobs);
    if (job != NULL) {
        (void)List_Remove(&pool->jobs, &job->link);
    }
    if (pool->jobs.count == 0u) {
        wtp_event_reset(&pool->available);
    }
    Lock_Unlock(&pool->lock);
    return job;
}

static void wtp_worker_run(wtp_worker *worker)
{
    worker_thread_pool *pool = NULL;

    if (worker == NULL) {
        return;
    }
    pool = worker->pool;
    if (pool == NULL) {
        return;
    }

    wtp_atomic_thread_id_store(&worker->thread_id,
                               wtp_current_thread_id());
    wtp_atomic_store(&worker->started, 1u);

    for (;;) {
        wtp_job *job = NULL;
        int wait_result = -1;

        wait_result = wtp_event_wait(&pool->available,
                                     WORKER_THREAD_POOL_WAIT_INFINITE);
        if (wait_result < 0) {
            /* Avoid a hot loop if the native event/handle becomes invalid. */
            break;
        }
        while ((job = wtp_queue_take(pool)) != NULL) {
            wtp_job_run(job);
            wtp_job_release(job);
        }
        if (wtp_atomic_load(&pool->stopping) != 0u) {
            break;
        }
    }
}

#if defined(_KERNEL_MODE)
static VOID wtp_worker_entry(PVOID context)
{
    wtp_worker_run((wtp_worker *)context);
    PsTerminateSystemThread(STATUS_SUCCESS);
}
#else
static DWORD WINAPI wtp_worker_entry(LPVOID context)
{
    wtp_worker_run((wtp_worker *)context);
    return 0;
}
#endif

static int wtp_worker_start(wtp_worker *worker)
{
#if defined(_KERNEL_MODE)
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    if (worker == NULL) {
        return 0;
    }
    status = PsCreateSystemThread(
        &worker->thread_handle, THREAD_ALL_ACCESS, NULL, NULL, NULL,
        wtp_worker_entry, worker);
    return NT_SUCCESS(status);
#else
    DWORD thread_id = 0u;

    if (worker == NULL) {
        return 0;
    }

    worker->thread_handle = CreateThread(NULL, 0, wtp_worker_entry, worker,
                                         0, &thread_id);
    if (worker->thread_handle == NULL) {
        return 0;
    }
    wtp_atomic_thread_id_store(&worker->thread_id, (ULONG_PTR)thread_id);
    return 1;
#endif
}

static void wtp_worker_join(wtp_worker *worker)
{
    if (worker == NULL || worker->thread_handle == NULL) {
        return;
    }
#if defined(_KERNEL_MODE)
    (void)ZwWaitForSingleObject(worker->thread_handle, FALSE, NULL);
    (void)ZwClose(worker->thread_handle);
#else
    (void)WaitForSingleObject(worker->thread_handle, INFINITE);
    (void)CloseHandle(worker->thread_handle);
#endif
    worker->thread_handle = NULL;
    wtp_atomic_thread_id_store(&worker->thread_id, 0u);
    wtp_atomic_store(&worker->started, 0u);
}

static void wtp_pool_free(worker_thread_pool *pool)
{
    wtp_event_destroy(&pool->stopped_event);
    wtp_event_destroy(&pool->available);
    Lock_Destroy(&pool->lock);
    wtp_free(pool->workers);
    wtp_free(pool);
}

worker_thread_pool *worker_thread_pool_create(uint32_t worker_count,
                                              uint32_t queue_limit)
{
    worker_thread_pool *pool = NULL;
    uint32_t index = 0u;
    size_t workers_size = 0u;
    int lock_ready = 0;
    int available_ready = 0;
    int stopped_ready = 0;

    if (worker_count == 0u || queue_limit == 0u) {
        return NULL;
    }
    workers_size = (size_t)worker_count * sizeof(wtp_worker);
    if (workers_size / (size_t)worker_count != sizeof(wtp_worker)) {
        return NULL;
    }

    pool = (worker_thread_pool *)wtp_alloc(sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    wtp_zero(pool, sizeof(*pool));

    pool->workers = (wtp_worker *)wtp_alloc(workers_size);
    if (pool->workers == NULL) {
        wtp_free(pool);
        return NULL;
    }
    wtp_zero(pool->workers, workers_size);

    lock_ready = Lock_Init(&pool->lock);
    available_ready = lock_ready && wtp_event_init(&pool->available, 0);
    stopped_ready = available_ready && wtp_event_init(&pool->stopped_event, 0);
    if (!List_Init(&pool->jobs) || !lock_ready || !available_ready ||
        !stopped_ready) {
        if (stopped_ready) {
            wtp_event_destroy(&pool->stopped_event);
        }
        if (available_ready) {
            wtp_event_destroy(&pool->available);
        }
        if (lock_ready) {
            Lock_Destroy(&pool->lock);
        }
        wtp_free(pool->workers);
        wtp_free(pool);
        return NULL;
    }

    pool->worker_limit = worker_count;
    pool->queue_limit = queue_limit;
    for (index = 0u; index < worker_count; ++index) {
        pool->workers[index].pool = pool;
        if (!wtp_worker_start(&pool->workers[index])) {
            uint32_t started = 0u;
            uint32_t joined = 0u;

            started = wtp_atomic_load(&pool->worker_count);

            wtp_atomic_store(&pool->stopping, 1u);
            wtp_event_set(&pool->available);
            for (joined = 0u; joined < started; ++joined) {
                wtp_worker_join(&pool->workers[joined]);
            }
            wtp_pool_free(pool);
            return NULL;
        }
        wtp_atomic_store(&pool->worker_count, index + 1u);
    }
    return pool;
}

int worker_thread_pool_is_worker(const worker_thread_pool *pool,
                                 ULONG_PTR thread_id)
{
    uint32_t index = 0u;

    if (pool == NULL || thread_id == 0u) {
        return 0;
    }
    for (index = 0u; index < pool->worker_limit; ++index) {
        const wtp_worker *worker = NULL;

        worker = &pool->workers[index];
        if (wtp_atomic_load(&worker->started) != 0u &&
            wtp_atomic_thread_id_load(&worker->thread_id) == thread_id) {
            return 1;
        }
    }
    return 0;
}

uint32_t worker_thread_pool_worker_count(const worker_thread_pool *pool)
{
    return pool == NULL ? 0u : wtp_atomic_load(&pool->worker_count);
}

int32_t worker_thread_pool_submit(worker_thread_pool *pool,
                                  worker_thread_pool_task_fn task,
                                  worker_thread_pool_task_fn finish,
                                  void *context,
                                  int wait_for_completion,
                                  uint32_t timeout_ms)
{
    wtp_job *job = NULL;
    int32_t status = WORKER_THREAD_POOL_INVALID_PARAMETER;

    if (pool == NULL || task == NULL) {
        return WORKER_THREAD_POOL_INVALID_PARAMETER;
    }

    job = wtp_job_create(task, finish, context,
                         wait_for_completion ? 2u : 1u);
    if (job == NULL) {
        return WORKER_THREAD_POOL_NO_MEMORY;
    }

    status = wtp_queue_put(pool, job);
    if (status != WORKER_THREAD_POOL_SUCCESS) {
        wtp_job_destroy(job);
        return status;
    }

    if (wait_for_completion) {
        status = wtp_job_wait(job, timeout_ms);
        wtp_job_release(job);
        return status;
    }
    return WORKER_THREAD_POOL_SUCCESS;
}

int32_t worker_thread_pool_stop(worker_thread_pool *pool)
{
    uint32_t count = 0u;
    uint32_t index = 0u;

    if (pool == NULL) {
        return WORKER_THREAD_POOL_INVALID_PARAMETER;
    }
    if (worker_thread_pool_is_worker(pool, wtp_current_thread_id())) {
        return WORKER_THREAD_POOL_INVALID_PARAMETER;
    }

    Lock_Exclusive(&pool->lock);
    if (wtp_atomic_load(&pool->stopped) != 0u) {
        Lock_Unlock(&pool->lock);
        return WORKER_THREAD_POOL_SUCCESS;
    }
    if (wtp_atomic_load(&pool->stop_started) != 0u) {
        Lock_Unlock(&pool->lock);
        return wtp_event_wait(&pool->stopped_event,
                              WORKER_THREAD_POOL_WAIT_INFINITE) > 0
                   ? WORKER_THREAD_POOL_SUCCESS
                   : WORKER_THREAD_POOL_STOPPED;
    }

    wtp_atomic_store(&pool->stop_started, 1u);
    wtp_atomic_store(&pool->stopping, 1u);
    count = wtp_atomic_load(&pool->worker_count);
    wtp_event_set(&pool->available);
    Lock_Unlock(&pool->lock);

    for (index = 0u; index < count; ++index) {
        wtp_worker_join(&pool->workers[index]);
    }

    Lock_Exclusive(&pool->lock);
    wtp_atomic_store(&pool->worker_count, 0u);
    wtp_atomic_store(&pool->stopped, 1u);
    wtp_event_set(&pool->stopped_event);
    Lock_Unlock(&pool->lock);
    return WORKER_THREAD_POOL_SUCCESS;
}

void worker_thread_pool_destroy(worker_thread_pool *pool)
{
    wtp_job *job = NULL;

    if (pool == NULL ||
        worker_thread_pool_is_worker(pool, wtp_current_thread_id())) {
        return;
    }
    if (worker_thread_pool_stop(pool) != WORKER_THREAD_POOL_SUCCESS) {
        return;
    }

    while ((job = wtp_queue_take(pool)) != NULL) {
        wtp_job_finish(job);
        wtp_atomic_store(&job->state, WTP_JOB_DONE);
        wtp_event_set(&job->done);
        wtp_job_release(job);
    }
    wtp_pool_free(pool);
}
