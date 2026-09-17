#include "alpc_port.h"
#include "def.h"

/* Keep this translation unit independent of the platform CRT. */
#ifndef _STDINT_H
#  define _STDINT_H
#endif
#if defined(_MSC_VER) && !defined(_STDINT_H_)
#  define _STDINT_H_
#endif
#include "libc.h"

/* All allocations go through the shared allocator.  The temporary guard only
 * prevents duplicate fallback scalar typedefs in no-WDK syntax-check builds. */
#if defined(_KERNEL_MODE) && !ALPC_PORT_HAS_PLATFORM_HEADERS && \
    !defined(ALLOCATOR_HAVE_WDK)
#  define ALLOCATOR_HAVE_WDK 1
#  define ALPC_PORT_DEFINED_ALLOCATOR_HAVE_WDK 1
#endif
#include "allocator.h"
#ifdef ALPC_PORT_DEFINED_ALLOCATOR_HAVE_WDK
#  undef ALLOCATOR_HAVE_WDK
#  undef ALPC_PORT_DEFINED_ALLOCATOR_HAVE_WDK
#endif

/* Value-initialize aggregates in both languages without triggering C++
 * -Wmissing-field-initializers while retaining C11 compatibility. */
#if defined(__cplusplus)
#  define ALPC_PORT_ZERO_INIT {}
#else
#  define ALPC_PORT_ZERO_INIT {0}
#endif

#if !ALPC_PORT_HAS_PLATFORM_HEADERS && defined(_MSC_VER)
#  include <intrin.h>
#endif

#if !defined(STATUS_SUCCESS)
#  define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#if !defined(STATUS_UNSUCCESSFUL)
#  define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#endif
#if !defined(STATUS_INVALID_PARAMETER)
#  define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif
#if !defined(STATUS_INFO_LENGTH_MISMATCH)
#  define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif
#if !defined(STATUS_DATA_ERROR)
#  define STATUS_DATA_ERROR ((NTSTATUS)0xC000003EL)
#endif
#if !defined(STATUS_INSUFFICIENT_RESOURCES)
#  define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)
#endif
#if !defined(STATUS_INTEGER_OVERFLOW)
#  define STATUS_INTEGER_OVERFLOW ((NTSTATUS)0xC0000095L)
#endif
#if !defined(STATUS_NOT_SUPPORTED)
#  define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#endif
#if !defined(STATUS_DEVICE_BUSY)
#  define STATUS_DEVICE_BUSY ((NTSTATUS)0xC00000E8L)
#endif
#if !defined(STATUS_TIMEOUT)
#  define STATUS_TIMEOUT ((NTSTATUS)0x00000102L)
#endif
#if !defined(STATUS_PORT_DISCONNECTED)
#  define STATUS_PORT_DISCONNECTED ((NTSTATUS)0xC0000037L)
#endif
#if !defined(NT_SUCCESS)
#  define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#endif

#define ALPC_PORT_STATUS(status) ((NTSTATUS)(status))

#define ALPC_PENDING_REPLY_CAPTURED          1L
#define ALPC_PENDING_REPLY_PENDING           2L
#define ALPC_PENDING_REPLY_CLAIMED_EARLY     3L
#define ALPC_PENDING_REPLY_CLAIMED           4L
#define ALPC_PENDING_REPLY_CLAIMED_READY     5L
#define ALPC_PENDING_REPLY_CLAIMED_CANCELLED 6L
#define ALPC_PORT_EMERGENCY_REPLY_SIZE        256U

/* Deferred replies live in a server-owned registry.  A token contains only
 * the record address and a non-repeating cookie; callers never own or
 * dereference the record.  The record itself owns one client lifetime pin. */
typedef struct _ALPC_PORT_PENDING_REPLY {
    /* Must remain first: list.h operates on object pointers directly. */
    LIST_ELEM listEntry;
    PALPC_PORT_SERVER_CLIENT client;
    ALPC_PORT_MESSAGE request;
    ULONG controlId;
    uint64_t cookie;
    LONG state;
} ALPC_PORT_PENDING_REPLY, *PALPC_PORT_PENDING_REPLY;

/* Publish the context state atomically so concurrent Close calls cannot both
 * tear down the same native handle while workers are still entering waits. */
static LONG alpc_state_load(const volatile LONG *state)
{
#if defined(_MSC_VER)
    return InterlockedCompareExchange((volatile LONG *)state, 0, 0);
#elif defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(state, __ATOMIC_ACQUIRE);
#else
    return *state;
#endif
}

static void alpc_state_store(volatile LONG *state, LONG value)
{
#if defined(_MSC_VER)
    (void)InterlockedExchange(state, value);
#elif defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(state, value, __ATOMIC_RELEASE);
#else
    *state = value;
#endif
}

static LONG alpc_state_compare_exchange(volatile LONG *state,
                                        LONG expected,
                                        LONG desired)
{
#if defined(_MSC_VER)
    return InterlockedCompareExchange(state, desired, expected);
#elif defined(__GNUC__) || defined(__clang__)
    LONG observed = expected;
    (void)__atomic_compare_exchange_n(state, &observed, desired, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return observed;
#else
    LONG observed = *state;
    if (observed == expected) {
        *state = desired;
    }
    return observed;
#endif
}

static int alpc_state_claim_close(volatile LONG *state)
{
    return state != (volatile LONG *)0 &&
           alpc_state_compare_exchange(state, 1, 0) == 1;
}

/* A lifetime's native synchronization object is destroyed during shutdown.
 * Serialize the short admission/destruction window globally so a thread that
 * observed the published context state just before shutdown cannot enter a
 * deleted CriticalSection.  Active operations are still tracked per object;
 * this gate is never held across a native call, callback, or rundown wait. */
static volatile LONG g_alpc_lifetime_admission = 0;

static void alpc_lifetime_global_lock(void)
{
    while (alpc_state_compare_exchange(&g_alpc_lifetime_admission, 0, 1) != 0) {
#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
        KeYieldProcessor();
#elif defined(_MSC_VER)
        YieldProcessor();
#endif
    }
}

static void alpc_lifetime_global_unlock(void)
{
    alpc_state_store(&g_alpc_lifetime_admission, 0);
}

/* Tokens can outlive a server instance in caller-owned storage.  A sequence
 * kept inside the context restarts after Close/Create and can therefore pair
 * a stale token with a newly allocated record at the same address.  Allocate
 * cookies from one process/module-wide monotonic sequence instead. */
static uint64_t g_alpc_pending_reply_cookie = 0U;
static uint64_t g_alpc_endpoint_cookie = 0U;
/* Native ALPC stores PortContext in one pointer-sized field.  Keep this
 * identity separate from both the freeable client record and the public
 * endpoint cookie.  Values are process/module-wide and consumed even when a
 * later accept step fails, so a delayed native message can never name a new
 * endpoint after allocator reuse or server recreation. */
static SIZE_T g_alpc_native_port_context_token = 0U;

static NTSTATUS alpc_next_pending_reply_cookie(uint64_t *cookie)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (!cookie) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    alpc_lifetime_global_lock();
    if (g_alpc_pending_reply_cookie == (uint64_t)-1) {
        status = STATUS_INTEGER_OVERFLOW;
    } else {
        ++g_alpc_pending_reply_cookie;
        *cookie = g_alpc_pending_reply_cookie;
    }
    alpc_lifetime_global_unlock();
    return status;
}

static NTSTATUS alpc_next_endpoint_cookie(uint64_t *cookie)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (!cookie) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    alpc_lifetime_global_lock();
    if (g_alpc_endpoint_cookie == (uint64_t)-1) {
        status = STATUS_INTEGER_OVERFLOW;
    } else {
        ++g_alpc_endpoint_cookie;
        *cookie = g_alpc_endpoint_cookie;
    }
    alpc_lifetime_global_unlock();
    return status;
}

static NTSTATUS alpc_next_native_port_context_token(PVOID *token)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (!token) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    *token = NULL;
    alpc_lifetime_global_lock();
    if (g_alpc_native_port_context_token == (SIZE_T)-1) {
        status = STATUS_INTEGER_OVERFLOW;
    } else {
        ++g_alpc_native_port_context_token;
        *token = (PVOID)(ULONG_PTR)g_alpc_native_port_context_token;
    }
    alpc_lifetime_global_unlock();
    return status;
}

static int alpc_lock_init(ALPC_PORT_LOCK *lock)
{
    if (!lock) {
        return 0;
    }
#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    ExInitializePushLock(&lock->native);
#elif !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    InitializeCriticalSection(&lock->native);
#else
    lock->native = 0;
#endif
    lock->initialized = 1;
    return 1;
}

static void alpc_lock_destroy(ALPC_PORT_LOCK *lock)
{
    if (!lock || !lock->initialized) {
        return;
    }
#if !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    DeleteCriticalSection(&lock->native);
#elif !defined(_KERNEL_MODE) && !ALPC_PORT_HAS_PLATFORM_HEADERS
    lock->native = 0;
#endif
    lock->initialized = 0;
}

static void alpc_lock_acquire(ALPC_PORT_LOCK *lock)
{
    if (!lock || !lock->initialized) {
        return;
    }
#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    ExAcquirePushLockExclusive(&lock->native);
#elif !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    EnterCriticalSection(&lock->native);
#elif defined(_MSC_VER)
    while (_InterlockedCompareExchange((volatile long *)&lock->native, 1, 0) != 0) {
        YieldProcessor();
    }
#elif defined(__GNUC__) || defined(__clang__)
    while (__sync_lock_test_and_set(&lock->native, 1U) != 0U) {
    }
#else
    while (lock->native) {
    }
    lock->native = 1U;
#endif
}

static void alpc_lock_release(ALPC_PORT_LOCK *lock)
{
    if (!lock || !lock->initialized) {
        return;
    }
#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    ExReleasePushLockExclusive(&lock->native);
#elif !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    LeaveCriticalSection(&lock->native);
#elif defined(_MSC_VER)
    _InterlockedExchange((volatile long *)&lock->native, 0);
#elif defined(__GNUC__) || defined(__clang__)
    __sync_lock_release(&lock->native);
#else
    lock->native = 0U;
#endif
}

#if !ALPC_PORT_HAS_PLATFORM_HEADERS
/* Serialize fallback lifetime state even when the host has no Windows
 * CriticalSection/rundown implementation available. */
static void alpc_lifetime_fallback_lock(ALPC_PORT_LIFETIME *life)
{
    if (!life) {
        return;
    }
#if defined(_MSC_VER)
    while (_InterlockedCompareExchange((volatile long *)&life->native, 1, 0) != 0) {
        YieldProcessor();
    }
#elif defined(__GNUC__) || defined(__clang__)
    while (__sync_lock_test_and_set(&life->native, 1U) != 0U) {
    }
#else
    while (life->native != 0U) {
    }
    life->native = 1U;
#endif

}

static void alpc_lifetime_fallback_unlock(ALPC_PORT_LIFETIME *life)
{
    if (!life) {
        return;
    }
#if defined(_MSC_VER)
    (void)_InterlockedExchange((volatile long *)&life->native, 0);
#elif defined(__GNUC__) || defined(__clang__)
    __sync_lock_release(&life->native);
#else
    life->native = 0U;
#endif
}
#endif

#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
/* Serialize the short closing/admission transition before touching rundown. */
static void alpc_lifetime_admission_lock(ALPC_PORT_LIFETIME *life)
{
    if (!life) {
        return;
    }
    while (InterlockedCompareExchange(&life->admission, 1, 0) != 0) {
        KeYieldProcessor();
    }
}

static void alpc_lifetime_admission_unlock(ALPC_PORT_LIFETIME *life)
{
    if (life) {
        (void)InterlockedExchange(&life->admission, 0);
    }
}
#endif

static int alpc_lifetime_init(ALPC_PORT_LIFETIME *life)
{
    if (!life) {
        return 0;
    }
    alpc_lifetime_global_lock();
    if (life->initialized) {
        alpc_lifetime_global_unlock();
        return 0;
    }
#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    ExInitializeRundownProtection(&life->rundown);
    life->admission = 0;
    life->closing = 0;
    life->initialized = 1;
#elif !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    InitializeCriticalSection(&life->native);
    InitializeConditionVariable(&life->idle);
    life->active = 0;
    life->closing = 0;
    life->initialized = 1;
#else
    life->native = 0;
    life->active = 0;
    life->closing = 0;
    life->initialized = 1;
#endif
    alpc_lifetime_global_unlock();
    return 1;
}

static int alpc_lifetime_acquire(ALPC_PORT_LIFETIME *life)
{
    int acquired = 0;

    if (!life) {
        return 0;
    }
    alpc_lifetime_global_lock();
    if (!life->initialized) {
        alpc_lifetime_global_unlock();
        return 0;
    }
#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    alpc_lifetime_admission_lock(life);
    if (life->initialized && life->closing == 0 &&
        ExAcquireRundownProtection(&life->rundown)) {
        acquired = 1;
    }
    alpc_lifetime_admission_unlock(life);
#elif !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    EnterCriticalSection(&life->native);
    if (life->initialized && !life->closing &&
        life->active != ~(uint32_t)0) {
        ++life->active;
        acquired = 1;
    }
    LeaveCriticalSection(&life->native);
#else
    alpc_lifetime_fallback_lock(life);
    if (life->initialized && !life->closing &&
        life->active != ~(uint32_t)0) {
        ++life->active;
        acquired = 1;
    }
    alpc_lifetime_fallback_unlock(life);
#endif
    alpc_lifetime_global_unlock();
    return acquired;
}

static void alpc_lifetime_release(ALPC_PORT_LIFETIME *life)
{
    if (!life) {
        return;
    }
#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    ExReleaseRundownProtection(&life->rundown);
#elif !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    EnterCriticalSection(&life->native);
    if (life->active) {
        --life->active;
    }
    if (life->closing && life->active == 0) {
        WakeAllConditionVariable(&life->idle);
    }
    LeaveCriticalSection(&life->native);
#else
    alpc_lifetime_fallback_lock(life);
    if (life->active) {
        --life->active;
    }
    alpc_lifetime_fallback_unlock(life);
#endif
}

static void alpc_lifetime_mark_closing(ALPC_PORT_LIFETIME *life)
{
    if (!life) {
        return;
    }
    alpc_lifetime_global_lock();
    if (!life->initialized) {
        alpc_lifetime_global_unlock();
        return;
    }
#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    alpc_lifetime_admission_lock(life);
    life->closing = 1;
    alpc_lifetime_admission_unlock(life);
#elif !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    EnterCriticalSection(&life->native);
    life->closing = 1;
    LeaveCriticalSection(&life->native);
#else
    alpc_lifetime_fallback_lock(life);
    life->closing = 1;
    alpc_lifetime_fallback_unlock(life);
#endif
    alpc_lifetime_global_unlock();
}

static void alpc_lifetime_wait(ALPC_PORT_LIFETIME *life)
{
    if (!life || !life->initialized) {
        return;
    }
#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    ExWaitForRundownProtectionRelease(&life->rundown);
#elif !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    EnterCriticalSection(&life->native);
    while (life->active != 0) {
        SleepConditionVariableCS(&life->idle, &life->native, INFINITE);
    }
    LeaveCriticalSection(&life->native);
#else
    for (;;) {
        uint32_t active = 0;
        alpc_lifetime_fallback_lock(life);
        active = life->active;
        alpc_lifetime_fallback_unlock(life);
        if (active == 0U) {
            break;
        }
#if defined(_MSC_VER)
        YieldProcessor();
#endif
    }
#endif
}

static void alpc_lifetime_destroy(ALPC_PORT_LIFETIME *life)
{
    if (!life) {
        return;
    }
    alpc_lifetime_global_lock();
#if !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    if (life->initialized) {
        DeleteCriticalSection(&life->native);
    }
#endif
    life->initialized = 0;
    alpc_lifetime_global_unlock();
}

static PALPC_PORT_SERVER_CLIENT alpc_find_client_locked(
    PALPC_PORT_SERVER_CONTEXT context, HANDLE clientPort);
static NTSTATUS alpc_drop_client(PALPC_PORT_SERVER_CONTEXT context,
                                 HANDLE clientPort, uint8_t disconnect,
                                 uint8_t notify);
static NTSTATUS alpc_drop_acquired_client(
    PALPC_PORT_SERVER_CONTEXT context,
    PALPC_PORT_SERVER_CLIENT client,
    uint8_t disconnect,
    uint8_t notify);

/* The context lock protects list membership; the lifetime object pins the
 * pointed-to record while a worker is using its handle.  Keeping these two
 * concerns separate lets shutdown remove a record first and wait for workers
 * without ever holding the non-recursive kernel PushLock. */
static PALPC_PORT_SERVER_CLIENT alpc_acquire_client(
    PALPC_PORT_SERVER_CONTEXT context, HANDLE clientPort)
{
    PALPC_PORT_SERVER_CLIENT current = NULL;

    if (!context || !clientPort || !context->lockWord.initialized) {
        return NULL;
    }
    alpc_lock_acquire(&context->lockWord);
    current = alpc_find_client_locked(context, clientPort);
    if (current && !alpc_lifetime_acquire(&current->lifetime)) {
        current = NULL;
    }
    alpc_lock_release(&context->lockWord);
    return current;
}

static PALPC_PORT_SERVER_CLIENT alpc_acquire_client_by_process(
    PALPC_PORT_SERVER_CONTEXT context, HANDLE process)
{
    PALPC_PORT_SERVER_CLIENT current = NULL;
    PALPC_PORT_SERVER_CLIENT match = NULL;
    uint32_t matches = 0U;
    if (!context || !context->lockWord.initialized || !process) {
        return NULL;
    }
    alpc_lock_acquire(&context->lockWord);
    for (current = (PALPC_PORT_SERVER_CLIENT)List_Head(&context->clientList);
         current != NULL;
         current = (PALPC_PORT_SERVER_CLIENT)List_Next(&current->listEntry)) {
        if (current->clientId.UniqueProcess == process) {
            match = current;
            ++matches;
        }
    }
    current = (matches == 1U) ? match : NULL;
    if (current && !alpc_lifetime_acquire(&current->lifetime)) {
        current = NULL;
    }
    alpc_lock_release(&context->lockWord);
    return current;
}

/* Native ALPC includes the originating thread in ClientId.  Prefer an exact
 * process+thread match so multiple endpoints from one process remain
 * distinguishable; process-only fallback is retained for older LPC doubles. */
static PALPC_PORT_SERVER_CLIENT alpc_acquire_client_by_id(
    PALPC_PORT_SERVER_CONTEXT context, const ALPC_PORT_CLIENT_ID *clientId)
{
    PALPC_PORT_SERVER_CLIENT current = NULL;
    PALPC_PORT_SERVER_CLIENT match = NULL;
    uint32_t matches = 0U;
    if (!context || !clientId || !context->lockWord.initialized) {
        return NULL;
    }
    alpc_lock_acquire(&context->lockWord);
    for (current = (PALPC_PORT_SERVER_CLIENT)List_Head(&context->clientList);
         current != NULL;
         current = (PALPC_PORT_SERVER_CLIENT)List_Next(&current->listEntry)) {
        if (current->clientId.UniqueProcess == clientId->UniqueProcess &&
            current->clientId.UniqueThread == clientId->UniqueThread) {
            match = current;
            ++matches;
        }
    }
    if (matches == 1U) {
        current = match;
    } else {
        alpc_lock_release(&context->lockWord);
        return alpc_acquire_client_by_process(context,
                                              clientId->UniqueProcess);
    }
    if (current && !alpc_lifetime_acquire(&current->lifetime)) {
        current = NULL;
    }
    alpc_lock_release(&context->lockWord);
    return current;
}

/* NtAlpcSendWaitReceivePort returns the opaque PortContext supplied to
 * NtAlpcAcceptConnectPort.  Match only that never-reused token: accepting a
 * record address here would reintroduce an ABA route after allocator reuse. */
static PALPC_PORT_SERVER_CLIENT alpc_acquire_client_by_context(
    PALPC_PORT_SERVER_CONTEXT context, PVOID portContext)
{
    PALPC_PORT_SERVER_CLIENT current = NULL;

    if (!context || !portContext || !context->lockWord.initialized) {
        return NULL;
    }
    alpc_lock_acquire(&context->lockWord);
    for (current = (PALPC_PORT_SERVER_CLIENT)List_Head(&context->clientList);
         current != NULL;
         current = (PALPC_PORT_SERVER_CLIENT)List_Next(&current->listEntry)) {
        if (current->portContextToken == portContext) {
            break;
        }
    }
    if (current && !alpc_lifetime_acquire(&current->lifetime)) {
        current = NULL;
    }
    alpc_lock_release(&context->lockWord);
    return current;
}

static void *alpc_alloc(SIZE_T size)
{
    if (!size) {
        return NULL;
    }
#if defined(_KERNEL_MODE)
    return Allocator_Malloc((BOOLEAN)1, (size_t)size, (ULONG)ALPC_PORT_POOL_TAG);
#else
    return Allocator_Malloc((size_t)size);
#endif
}

static void alpc_free(void *memory)
{
    if (!memory) {
        return;
    }
#if defined(_KERNEL_MODE)
    Allocator_Free(memory, (ULONG)ALPC_PORT_POOL_TAG);
#else
    Allocator_Free(memory);
#endif
}

static NTSTATUS alpc_query_system_time(
    AlpcPort_NtQuerySystemTime querySystemTime,
    PLARGE_INTEGER systemTime)
{
    if (!systemTime) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    if (querySystemTime) {
        return querySystemTime(systemTime);
    }
#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    KeQuerySystemTime(systemTime);
    return STATUS_SUCCESS;
#elif !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    {
        FILETIME fileTime = ALPC_PORT_ZERO_INIT;
        uint64_t value = 0U;
        GetSystemTimeAsFileTime(&fileTime);
        value = ((uint64_t)fileTime.dwHighDateTime << 32) |
                (uint64_t)fileTime.dwLowDateTime;
        systemTime->QuadPart = (LONGLONG)value;
        return STATUS_SUCCESS;
    }
#else
    return STATUS_NOT_SUPPORTED;
#endif
}

NTSTATUS AlpcPort_NormalizeTimeout(
    AlpcPort_NtQuerySystemTime querySystemTime,
    const LARGE_INTEGER *timeout,
    PLARGE_INTEGER deadline)
{
    const LONGLONG maximum = (LONGLONG)0x7fffffffffffffffLL;
    const LONGLONG minimum = (LONGLONG)(-0x7fffffffffffffffLL - 1LL);
    LARGE_INTEGER now = ALPC_PORT_ZERO_INIT;
    LONGLONG interval = 0;
    NTSTATUS status = STATUS_SUCCESS;

    if (!timeout || !deadline) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    if (timeout->QuadPart == 0) {
        *deadline = *timeout;
        return STATUS_TIMEOUT;
    }
    if (timeout->QuadPart > 0) {
        *deadline = *timeout;
        return STATUS_SUCCESS;
    }
    if (timeout->QuadPart == minimum) {
        return STATUS_INTEGER_OVERFLOW;
    }
    interval = -timeout->QuadPart;
    status = alpc_query_system_time(querySystemTime, &now);
    if (status != STATUS_SUCCESS) {
        return status;
    }
    if (now.QuadPart > maximum - interval) {
        return STATUS_INTEGER_OVERFLOW;
    }
    deadline->QuadPart = now.QuadPart + interval;
    return STATUS_SUCCESS;
}

NTSTATUS AlpcPort_RemainingTimeout(
    AlpcPort_NtQuerySystemTime querySystemTime,
    const LARGE_INTEGER *deadline,
    PLARGE_INTEGER remaining)
{
    const LONGLONG maximum = (LONGLONG)0x7fffffffffffffffLL;
    LARGE_INTEGER now = ALPC_PORT_ZERO_INIT;
    LONGLONG interval = 0;
    NTSTATUS status = STATUS_SUCCESS;

    if (!deadline || !remaining) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    remaining->QuadPart = 0;
    if (deadline->QuadPart == 0) {
        return STATUS_TIMEOUT;
    }
    status = alpc_query_system_time(querySystemTime, &now);
    if (status != STATUS_SUCCESS) {
        return status;
    }
    if (deadline->QuadPart <= now.QuadPart) {
        return STATUS_TIMEOUT;
    }
    if (now.QuadPart < 0 &&
        deadline->QuadPart > maximum + now.QuadPart) {
        return STATUS_INTEGER_OVERFLOW;
    }
    interval = deadline->QuadPart - now.QuadPart;
    remaining->QuadPart = -interval;
    return STATUS_SUCCESS;
}

static NTSTATUS alpc_refresh_timeout(
    AlpcPort_NtQuerySystemTime querySystemTime,
    const LARGE_INTEGER *deadline,
    PLARGE_INTEGER remaining,
    PLARGE_INTEGER *timeoutArgument)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (!remaining || !timeoutArgument) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    *timeoutArgument = NULL;
    if (!deadline) {
        return STATUS_SUCCESS;
    }
    status = AlpcPort_RemainingTimeout(querySystemTime, deadline, remaining);
    if (status == STATUS_SUCCESS) {
        *timeoutArgument = remaining;
    }
    return status;
}

static int alpc_client_api_valid(const ALPC_PORT_CLIENT_APIS *api)
{
    return api && api->pfnRtlInitAnsiString &&
           api->pfnRtlAnsiStringToUnicodeString && api->pfnRtlFreeUnicodeString &&
           api->pfnNtClose && api->pfnNtAlpcConnectPort &&
           api->pfnNtAlpcSendWaitReceivePort && api->pfnNtAlpcDisconnectPort;
}

static int alpc_server_api_valid(const ALPC_PORT_SERVER_APIS *api)
{
    return api && api->pfnRtlInitAnsiString &&
           api->pfnRtlAnsiStringToUnicodeString && api->pfnRtlFreeUnicodeString &&
           api->pfnNtClose && api->pfnNtAlpcCreatePort &&
           api->pfnNtAlpcAcceptConnectPort &&
           api->pfnNtAlpcSendWaitReceivePort && api->pfnNtAlpcDisconnectPort;
}

static int alpc_name_valid(const ALPC_PORT_NAME *name)
{
    size_t length = 0;

    if (!name) {
        return 0;
    }
    length = libc_strnlen_s(name->name, sizeof(name->name));
    return length > 0 && length < sizeof(name->name);
}

static SIZE_T alpc_normalize_max_message_length(SIZE_T configured)
{
    return configured ? configured : ALPC_PORT_DEFAULT_MAX_MESSAGE_LENGTH;
}

static int alpc_max_message_length_valid(SIZE_T length)
{
    return length >= (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET &&
           length <= ALPC_PORT_HARD_MAX_MESSAGE_LENGTH && length <= (SIZE_T)0xffffU;
}

static void alpc_init_qos(SECURITY_QUALITY_OF_SERVICE *qos)
{
    if (!qos) {
        return;
    }
    if (!qos->Length) {
        qos->Length = (ULONG)sizeof(*qos);
        qos->ImpersonationLevel = SecurityImpersonation;
        qos->ContextTrackingMode = (BOOLEAN)SECURITY_DYNAMIC_TRACKING;
        qos->EffectiveOnly = (BOOLEAN)0;
    }
}

static void alpc_init_port_attributes(PALPC_PORT_SDK_ATTRIBUTES attributes,
                                       SIZE_T maxMessageLength,
                                       ULONG flags,
                                       SIZE_T maxPoolUsage,
                                       SIZE_T maxSectionSize,
                                       SIZE_T maxViewSize,
                                       SIZE_T maxTotalSectionSize,
                                       const SECURITY_QUALITY_OF_SERVICE *qos)
{
    if (!attributes) {
        return;
    }
    libc_memset(attributes, 0, sizeof(*attributes));
    attributes->Flags = flags;
    attributes->MaxMessageLength = maxMessageLength;
    attributes->MaxPoolUsage = maxPoolUsage;
    attributes->MaxSectionSize = maxSectionSize;
    attributes->MaxViewSize = maxViewSize;
    attributes->MaxTotalSectionSize = maxTotalSectionSize;
    if (qos) {
        attributes->SecurityQos = *qos;
    }
    alpc_init_qos(&attributes->SecurityQos);
}

static void alpc_init_object_attributes(POBJECT_ATTRIBUTES attributes,
                                         PUNICODE_STRING objectName,
                                         PSECURITY_DESCRIPTOR securityDescriptor)
{
    if (!attributes) {
        return;
    }
    libc_memset(attributes, 0, sizeof(*attributes));
    attributes->Length = (ULONG)sizeof(*attributes);
    attributes->Attributes = OBJ_CASE_INSENSITIVE;
    attributes->ObjectName = objectName;
    attributes->SecurityDescriptor = securityDescriptor;
}

static NTSTATUS alpc_build_unicode_name(AlpcPort_RtlInitAnsiString initAnsiString,
                                        AlpcPort_RtlAnsiStringToUnicodeString toUnicodeString,
                                        const ALPC_PORT_NAME *name,
                                        PUNICODE_STRING unicodeName)
{
    ANSI_STRING ansiName = ALPC_PORT_ZERO_INIT;

    if (!initAnsiString || !toUnicodeString || !alpc_name_valid(name) || !unicodeName) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    libc_memset(&ansiName, 0, sizeof(ansiName));
    libc_memset(unicodeName, 0, sizeof(*unicodeName));
    initAnsiString(&ansiName, name->name);
    return toUnicodeString(unicodeName, &ansiName, (BOOLEAN)1);
}

static int alpc_set_frame_length(PALPC_PORT_FRAME frame,
                                 SIZE_T capacity,
                                 ULONG payloadLength)
{
    SIZE_T totalLength = 0;

    if (!frame || capacity < (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET ||
        (SIZE_T)payloadLength > capacity - (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET) {
        return 0;
    }
    totalLength = (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET + (SIZE_T)payloadLength;
    if (totalLength > (SIZE_T)0xffffU) {
        return 0;
    }
    frame->header.u1.s1.DataLength =
        (USHORT)(totalLength - sizeof(ALPC_PORT_MESSAGE));
    frame->header.u1.s1.TotalLength = (USHORT)totalLength;
    frame->PayloadLength = payloadLength;
    return 1;
}

static int alpc_init_frame(PALPC_PORT_FRAME frame,
                           SIZE_T capacity,
                           USHORT type,
                           ULONG controlId,
                           ULONG flags,
                           const void *payload,
                           ULONG payloadLength)
{
    if (!frame || capacity < (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET ||
        (payloadLength && !payload)) {
        return 0;
    }
    libc_memset(frame, 0, (size_t)capacity);
    frame->header.u2.s2.Type = type;
    frame->ControlId = controlId;
    frame->Flags = flags;
    if (!alpc_set_frame_length(frame, capacity, payloadLength)) {
        return 0;
    }
    if (payloadLength) {
        libc_memcpy(frame->data, payload, payloadLength);
    }
    return 1;
}

static int alpc_frame_valid(const PALPC_PORT_FRAME frame, SIZE_T capacity)
{
    SIZE_T totalLength = 0;

    /* PayloadLength is part of the SDK frame, so the caller must provide the
     * complete fixed prefix before any of those fields are inspected. */
    if (!frame || capacity < (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET) {
        return 0;
    }
    totalLength = frame->header.u1.s1.TotalLength;
    if (totalLength < (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET ||
        totalLength > capacity ||
        frame->header.u1.s1.DataLength !=
            totalLength - sizeof(ALPC_PORT_MESSAGE) ||
        frame->PayloadLength !=
            totalLength - (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET) {
        return 0;
    }
    return 1;
}

/* Convert a received request into the smallest possible reply while keeping
 * the Native routing fields (MessageId/ClientId) supplied by the kernel.  The
 * helper is used for malformed synchronous requests so a peer cannot remain
 * blocked forever waiting for a reply that the server silently discarded. */
static int alpc_prepare_error_reply(PALPC_PORT_FRAME frame, SIZE_T capacity,
                                    NTSTATUS errorStatus)
{
    if (!frame || capacity < (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET) {
        return 0;
    }
    /* NtAlpcSendWaitReceivePort owns the outbound Type field.  Pre-filling a
     * legacy LPC_REPLY value makes cross-process ALPC reject the message with
     * STATUS_LPC_REQUESTS_NOT_ALLOWED on current Windows releases. */
    frame->header.u2.ZeroInit = 0U;
    frame->Flags = 0;
    frame->PayloadLength = 0;
    frame->Status = errorStatus;
    return alpc_set_frame_length(frame, capacity, 0);
}

static NTSTATUS alpc_send_error_reply(const ALPC_PORT_SERVER_CONTEXT *context,
                                      HANDLE clientPort,
                                      PALPC_PORT_FRAME frame,
                                      NTSTATUS errorStatus,
                                      uint8_t *replyFailed)
{
    NTSTATUS sendStatus = STATUS_DATA_ERROR;
    LARGE_INTEGER noWait = ALPC_PORT_ZERO_INIT;

    if (replyFailed) {
        *replyFailed = 0U;
    }
    if (!context || !context->api.pfnNtAlpcSendWaitReceivePort ||
        !alpc_prepare_error_reply(frame, context->maxMessageLength,
                                  errorStatus)) {
        if (replyFailed) {
            *replyFailed = 1U;
        }
        return STATUS_DATA_ERROR;
    }
    sendStatus = context->api.pfnNtAlpcSendWaitReceivePort(
        clientPort, ALPC_PORT_SEND_FLAG_REPLY_MESSAGE, &frame->header, NULL,
        NULL, NULL, NULL, &noWait);
    if (replyFailed &&
        (sendStatus == STATUS_TIMEOUT || !NT_SUCCESS(sendStatus))) {
        *replyFailed = 1U;
    }
    return (sendStatus == STATUS_TIMEOUT || !NT_SUCCESS(sendStatus))
               ? sendStatus : errorStatus;
}

static int alpc_message_type_is(USHORT actual, ULONG expected);
static int alpc_received_frame_valid(const PALPC_PORT_FRAME frame,
                                     SIZE_T receivedLength, SIZE_T capacity);

/* A connection request must be explicitly rejected after it has been
 * dequeued.  Otherwise the client can remain pending even though parsing
 * failed and the server returned an error to its worker. */
static NTSTATUS alpc_reject_connection(const ALPC_PORT_SERVER_CONTEXT *context,
                                       PALPC_PORT_FRAME frame)
{
    ALPC_PORT_MESSAGE normalized = ALPC_PORT_ZERO_INIT;
    HANDLE rejectedPort = NULL;
    NTSTATUS status = STATUS_DATA_ERROR;

    if (!context || !frame || !context->api.pfnNtAlpcAcceptConnectPort ||
        !context->connectionPortHandle) {
        return STATUS_DATA_ERROR;
    }
    /* The native reject path still parses the PORT_MESSAGE.  Normalize the
     * attacker-controlled length/type fields while preserving the routing
     * identifiers needed to match the pending connection. */
    normalized = frame->header;
    normalized.u1.s1.TotalLength = (USHORT)sizeof(ALPC_PORT_MESSAGE);
    normalized.u1.s1.DataLength = 0;
    normalized.u2.s2.Type = (USHORT)ALPC_PORT_MESSAGE_TYPE_CONNECTION_REQUEST;
    normalized.u2.s2.DataInfoOffset = 0;
    status = context->api.pfnNtAlpcAcceptConnectPort(
        &rejectedPort, context->connectionPortHandle, 0, NULL, NULL, NULL,
        &normalized, NULL, (BOOLEAN)0);
    if (rejectedPort && context->api.pfnNtClose) {
        (void)context->api.pfnNtClose(rejectedPort);
    }
    return status;
}

/* BufferLength is an in/out capacity for the native ALPC calls.  Some Windows
 * builds leave it unchanged on success, so PORT_MESSAGE.TotalLength is the
 * authoritative frame length.  The supplied length remains an upper bound;
 * all SDK payload fields are validated against TotalLength below. */
static int alpc_received_frame_valid(const PALPC_PORT_FRAME frame,
                                     SIZE_T receivedLength,
                                     SIZE_T capacity)
{
    SIZE_T total = 0U;
    if (!frame || receivedLength > capacity) {
        return 0;
    }
    total = (SIZE_T)frame->header.u1.s1.TotalLength;
    if (total < (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET ||
        total > receivedLength || total > capacity) {
        return 0;
    }
    return alpc_frame_valid(frame, total);
}

static int alpc_message_type_is(USHORT actual, ULONG expected)
{
    /* PORT_MESSAGE.Type stores the LPC message kind in its low 12 bits.  The
     * high nibble contains Native transport flags and differs across paths:
     * native x64 commonly reports 0x2xxx, while WOW64 can report 0x3xxx.
     * Compare only the documented message-kind portion on every receive path. */
    return ((ULONG)actual & 0x0fffU) == (expected & 0x0fffU);
}

/* Native ALPC control notifications are PORT_MESSAGE headers rather than SDK
 * frames.  Match their low 12-bit kind so bare 0x000b/0x000c and flagged
 * 0x200b/0x200c or 0x300b/0x300c forms are consumed identically. */
static int alpc_native_control_type(USHORT actual)
{
    return alpc_message_type_is(actual, ALPC_PORT_MESSAGE_TYPE_PORT_CLOSED) ||
           alpc_message_type_is(actual, 0x000BU) ||
           alpc_message_type_is(actual, 0x000CU);
}

static int alpc_native_connection_complete_type(USHORT actual)
{
    return alpc_message_type_is(actual, 0x000BU);
}

static void alpc_reset_client_context(PALPC_PORT_CLIENT_CONTEXT context)
{
    if (context) {
        libc_memset(context, 0, sizeof(*context));
    }
}

static void alpc_reset_server_context(PALPC_PORT_SERVER_CONTEXT context)
{
    if (context) {
        libc_memset(context, 0, sizeof(*context));
    }
}

static PALPC_PORT_SERVER_CLIENT alpc_find_client_locked(PALPC_PORT_SERVER_CONTEXT context,
                                                         HANDLE clientPort)
{
    PALPC_PORT_SERVER_CLIENT current = NULL;

    for (current = (PALPC_PORT_SERVER_CLIENT)List_Head(&context->clientList);
         current;
         current = (PALPC_PORT_SERVER_CLIENT)List_Next(&current->listEntry)) {
        if (current->portHandle == clientPort) {
            return current;
        }
    }
    return NULL;
}

static PALPC_PORT_PENDING_REPLY alpc_find_pending_pointer_locked(
    PALPC_PORT_SERVER_CONTEXT context, PVOID pointer)
{
    PALPC_PORT_PENDING_REPLY current = NULL;

    if (!context || !pointer) {
        return NULL;
    }
    for (current = (PALPC_PORT_PENDING_REPLY)List_Head(&context->pendingReplies);
         current != NULL;
         current = (PALPC_PORT_PENDING_REPLY)List_Next(&current->listEntry)) {
        if ((PVOID)current == pointer) {
            return current;
        }
    }
    return NULL;
}

static PALPC_PORT_PENDING_REPLY alpc_find_pending_locked(
    PALPC_PORT_SERVER_CONTEXT context, const ALPC_PORT_REPLY_TOKEN *token)
{
    PALPC_PORT_PENDING_REPLY pending = NULL;

    if (!context || !token || !token->opaque || token->cookie == 0U) {
        return NULL;
    }
    pending = alpc_find_pending_pointer_locked(context, token->opaque);
    return (pending && pending->cookie == token->cookie) ? pending : NULL;
}

static int alpc_pending_is_claimed(LONG state)
{
    return state == ALPC_PENDING_REPLY_CLAIMED_EARLY ||
           state == ALPC_PENDING_REPLY_CLAIMED ||
           state == ALPC_PENDING_REPLY_CLAIMED_READY ||
           state == ALPC_PENDING_REPLY_CLAIMED_CANCELLED;
}

static void alpc_release_pending_record(PALPC_PORT_PENDING_REPLY pending)
{
    if (!pending) {
        return;
    }
    if (pending->client) {
        alpc_lifetime_release(&pending->client->lifetime);
    }
    alpc_free(pending);
}

static void alpc_release_pending_chain(PALPC_PORT_PENDING_REPLY pending)
{
    PALPC_PORT_PENDING_REPLY next = NULL;

    while (pending) {
        next = (PALPC_PORT_PENDING_REPLY)pending->listEntry.next;
        pending->listEntry.next = NULL;
        pending->listEntry.prev = NULL;
        alpc_release_pending_record(pending);
        pending = next;
    }
}

/* Caller owns context->lockWord.  Unclaimed records are detached and returned
 * for release outside the lock.  A claimed record belongs to ServerReply, so
 * cancellation only changes its state until that claimant removes it. */
static PALPC_PORT_PENDING_REPLY alpc_cancel_pending_locked(
    PALPC_PORT_SERVER_CONTEXT context,
    PALPC_PORT_SERVER_CLIENT client,
    int forceClaimed)
{
    PALPC_PORT_PENDING_REPLY current = NULL;
    PALPC_PORT_PENDING_REPLY next = NULL;
    PALPC_PORT_PENDING_REPLY releaseList = NULL;

    for (current = (PALPC_PORT_PENDING_REPLY)List_Head(&context->pendingReplies);
         current != NULL;
         current = next) {
        next = (PALPC_PORT_PENDING_REPLY)List_Next(&current->listEntry);
        if (client && current->client != client) {
            continue;
        }
        if (!forceClaimed && alpc_pending_is_claimed(current->state)) {
            current->state = ALPC_PENDING_REPLY_CLAIMED_CANCELLED;
            continue;
        }
        if (current->client &&
            current->client->currentPendingReply == (PVOID)current) {
            current->client->currentPendingReply = NULL;
            current->client->currentRequestCaptured = 0U;
        }
        if (List_Remove(&context->pendingReplies, &current->listEntry)) {
            current->state = ALPC_PENDING_REPLY_CLAIMED_CANCELLED;
            current->listEntry.next = (LIST_ELEM *)releaseList;
            current->listEntry.prev = NULL;
            releaseList = current;
        }
    }
    return releaseList;
}

static void alpc_cancel_pending_for_client(
    PALPC_PORT_SERVER_CONTEXT context,
    PALPC_PORT_SERVER_CLIENT client,
    int forceClaimed)
{
    PALPC_PORT_PENDING_REPLY releaseList = NULL;

    if (!context || !client || !context->lockWord.initialized) {
        return;
    }
    alpc_lock_acquire(&context->lockWord);
    releaseList = alpc_cancel_pending_locked(context, client, forceClaimed);
    alpc_lock_release(&context->lockWord);
    alpc_release_pending_chain(releaseList);
}

static void alpc_cancel_all_pending(PALPC_PORT_SERVER_CONTEXT context,
                                    int forceClaimed)
{
    PALPC_PORT_PENDING_REPLY releaseList = NULL;

    if (!context || !context->lockWord.initialized) {
        return;
    }
    alpc_lock_acquire(&context->lockWord);
    releaseList = alpc_cancel_pending_locked(context, NULL, forceClaimed);
    alpc_lock_release(&context->lockWord);
    alpc_release_pending_chain(releaseList);
}

/* Reconcile a callback's return value with any token captured during that
 * callback.  This closes the race where the reply worker claims the token
 * before onSyncRequest has returned STATUS_PENDING. */
static NTSTATUS alpc_finish_sync_callback(
    PALPC_PORT_SERVER_CONTEXT context,
    PALPC_PORT_SERVER_CLIENT client,
    NTSTATUS callbackStatus)
{
    PALPC_PORT_PENDING_REPLY pending = NULL;
    PALPC_PORT_PENDING_REPLY releasePending = NULL;
    uint8_t captured = 0U;
    NTSTATUS result = callbackStatus;

    if (!context || !client) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    alpc_lock_acquire(&context->lockWord);
    captured = client->currentRequestCaptured;
    pending = alpc_find_pending_pointer_locked(
        context, client->currentPendingReply);

    if (callbackStatus == STATUS_PENDING) {
        if (!captured || !pending || pending->client != client) {
            result = STATUS_INVALID_PARAMETER;
        } else if (pending->state == ALPC_PENDING_REPLY_CAPTURED) {
            pending->state = ALPC_PENDING_REPLY_PENDING;
        } else if (pending->state == ALPC_PENDING_REPLY_CLAIMED_EARLY) {
            pending->state = ALPC_PENDING_REPLY_CLAIMED_READY;
        } else if (pending->state !=
                   ALPC_PENDING_REPLY_CLAIMED_CANCELLED) {
            pending->state = ALPC_PENDING_REPLY_CLAIMED_CANCELLED;
            result = STATUS_INVALID_PARAMETER;
        }
    } else if (captured && pending && pending->client == client) {
        if (pending->state == ALPC_PENDING_REPLY_CAPTURED ||
            pending->state == ALPC_PENDING_REPLY_PENDING) {
            if (List_Remove(&context->pendingReplies,
                            &pending->listEntry)) {
                pending->state = ALPC_PENDING_REPLY_CLAIMED_CANCELLED;
                releasePending = pending;
            }
        } else if (alpc_pending_is_claimed(pending->state)) {
            pending->state = ALPC_PENDING_REPLY_CLAIMED_CANCELLED;
        }
    }

    client->currentRequestValid = 0U;
    client->currentRequestCaptured = 0U;
    client->currentPendingReply = NULL;
    client->currentControlId = 0U;
    libc_memset(&client->currentRequest, 0, sizeof(client->currentRequest));
    alpc_lock_release(&context->lockWord);

    alpc_release_pending_record(releasePending);
    return result;
}

/* On success the caller receives one client lifetime pin.  Acquiring it while
 * the record is still protected by the list lock closes the publication gap
 * in which a concurrent explicit disconnect could otherwise free the record
 * before onPostConnect begins. */
static int alpc_add_client(PALPC_PORT_SERVER_CONTEXT context,
                           PALPC_PORT_SERVER_CLIENT client)
{
    PALPC_PORT_SERVER_CLIENT current = NULL;
    int inserted = 0;
    uint64_t endpointCookie = 0U;

    if (!context || !client) {
        return 0;
    }
    if (!client->portContextToken) {
        alpc_lifetime_destroy(&client->lifetime);
        alpc_lock_destroy(&client->receiveLock);
        return 0;
    }
    if (alpc_next_endpoint_cookie(&endpointCookie) != STATUS_SUCCESS) {
        alpc_lifetime_destroy(&client->lifetime);
        alpc_lock_destroy(&client->receiveLock);
        return 0;
    }
    client->endpointCookie = endpointCookie;
    if (!client->receiveLock.initialized && !alpc_lock_init(&client->receiveLock)) {
        return 0;
    }
    if (!client->lifetime.initialized && !alpc_lifetime_init(&client->lifetime)) {
        alpc_lock_destroy(&client->receiveLock);
        return 0;
    }
    alpc_lock_acquire(&context->lockWord);
    for (current = (PALPC_PORT_SERVER_CLIENT)List_Head(&context->clientList);
         current != NULL;
         current = (PALPC_PORT_SERVER_CLIENT)List_Next(&current->listEntry)) {
        if (current->portContextToken == client->portContextToken) {
            break;
        }
    }
    if ((!context->maxClients || context->clientCount < context->maxClients) &&
        !alpc_find_client_locked(context, client->portHandle) &&
        current == NULL &&
        List_Insert_After(&context->clientList, List_Tail(&context->clientList),
                          &client->listEntry)) {
        if (context->clientCount != (uint32_t)-1 &&
            alpc_lifetime_acquire(&client->lifetime)) {
            ++context->clientCount;
            inserted = 1;
        } else {
            List_Remove(&context->clientList, &client->listEntry);
        }
    }
    alpc_lock_release(&context->lockWord);
    if (!inserted) {
        alpc_lifetime_destroy(&client->lifetime);
        alpc_lock_destroy(&client->receiveLock);
    }
    return inserted;
}

static PALPC_PORT_SERVER_CLIENT alpc_detach_client(PALPC_PORT_SERVER_CONTEXT context,
                                                    HANDLE clientPort)
{
    PALPC_PORT_SERVER_CLIENT client = NULL;

    if (!context || !clientPort) {
        return NULL;
    }
    alpc_lock_acquire(&context->lockWord);
    client = alpc_find_client_locked(context, clientPort);
    if (client && List_Remove(&context->clientList, &client->listEntry)) {
        if (context->clientCount) {
            --context->clientCount;
        }
    } else {
        client = NULL;
    }
    alpc_lock_release(&context->lockWord);
    return client;
}

static void alpc_snapshot_events(PALPC_PORT_SERVER_CONTEXT context,
                                  ALPC_PORT_SERVER_EVENTS *events)
{
    if (!context || !events) {
        return;
    }
    alpc_lock_acquire(&context->lockWord);
    *events = context->events;
    alpc_lock_release(&context->lockWord);
}

static void alpc_close_client_handle(PALPC_PORT_SERVER_CONTEXT context,
                                      HANDLE clientPort,
                                      uint8_t disconnect)
{
    if (!context || !clientPort) {
        return;
    }
    if (disconnect && context->api.pfnNtAlpcDisconnectPort) {
        context->api.pfnNtAlpcDisconnectPort(clientPort, 0);
    }
    if (context->api.pfnNtClose) {
        context->api.pfnNtClose(clientPort);
    }
}

static void alpc_release_client_info(PALPC_PORT_SERVER_CLIENT client)
{
    if (!client) {
        return;
    }
    alpc_lock_destroy(&client->receiveLock);
    alpc_lifetime_destroy(&client->lifetime);
    alpc_free(client);
}

NTSTATUS AlpcPort_ServerCreate(PALPC_PORT_SERVER_CONFIG config,
                               PALPC_PORT_SERVER_CONTEXT context)
{
    ALPC_PORT_SDK_ATTRIBUTES portAttributes = ALPC_PORT_ZERO_INIT;
    OBJECT_ATTRIBUTES objectAttributes = ALPC_PORT_ZERO_INIT;
    SECURITY_QUALITY_OF_SERVICE securityQos = ALPC_PORT_ZERO_INIT;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    SIZE_T maxMessageLength = 0;

    if (!config || !context || !alpc_name_valid(&config->portName) ||
        alpc_state_load(&context->initialized) != 0 || context->connectionPortHandle ||
        context->unicodeName.Buffer || context->lockWord.initialized ||
        context->connectionReceiveLock.initialized ||
        context->lifetime.initialized || !alpc_server_api_valid(&config->api)) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    maxMessageLength = alpc_normalize_max_message_length(config->maxMessageLength);
    if (!alpc_max_message_length_valid(maxMessageLength)) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    alpc_reset_server_context(context);
    context->api = config->api;
    context->maxClients = config->maxClients;
    if (!alpc_lifetime_init(&context->lifetime) ||
        !List_Init(&context->clientList) ||
        !List_Init(&context->pendingReplies) ||
        !alpc_lock_init(&context->lockWord)) {
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_server_context(context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!alpc_lock_init(&context->connectionReceiveLock)) {
        alpc_lock_destroy(&context->lockWord);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_server_context(context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = alpc_build_unicode_name(context->api.pfnRtlInitAnsiString,
                                      context->api.pfnRtlAnsiStringToUnicodeString,
                                      &config->portName, &context->unicodeName);
    if (!NT_SUCCESS(status)) {
        alpc_lock_destroy(&context->connectionReceiveLock);
        alpc_lock_destroy(&context->lockWord);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_server_context(context);
        return status;
    }
    alpc_init_object_attributes(&objectAttributes, &context->unicodeName,
                                config->securityDescriptor);
    securityQos = config->securityQos;
    alpc_init_qos(&securityQos);
    alpc_init_port_attributes(&portAttributes, maxMessageLength,
                              config->portFlags |
                                  ALPC_PORT_FLAG_ALLOW_LPC_REQUESTS,
                              config->maxPoolUsage, config->maxSectionSize,
                              config->maxViewSize, config->maxTotalSectionSize,
                              &securityQos);
    status = context->api.pfnNtAlpcCreatePort(&context->connectionPortHandle,
                                               &objectAttributes, &portAttributes);
    if (!NT_SUCCESS(status) || !context->connectionPortHandle) {
        if (NT_SUCCESS(status)) {
            status = STATUS_DATA_ERROR;
        }
        if (context->connectionPortHandle && context->api.pfnNtClose) {
            (void)context->api.pfnNtClose(context->connectionPortHandle);
        }
        context->api.pfnRtlFreeUnicodeString(&context->unicodeName);
        alpc_lock_destroy(&context->connectionReceiveLock);
        alpc_lock_destroy(&context->lockWord);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_server_context(context);
        return status;
    }
    context->portAttributes = portAttributes;
    context->maxMessageLength = maxMessageLength;
    alpc_state_store(&context->initialized, 1);
    return STATUS_SUCCESS;
}

void AlpcPort_ServerClose(PALPC_PORT_SERVER_CONTEXT context)
{
    PALPC_PORT_SERVER_CLIENT client = NULL;
    PALPC_PORT_SERVER_CLIENT orphanClients = NULL;
    ALPC_PORT_SERVER_EVENTS closeEvents = ALPC_PORT_ZERO_INIT;
    LIST closingClients = ALPC_PORT_ZERO_INIT;

    if (!context || !alpc_state_claim_close(&context->initialized)) {
        return;
    }
    (void)List_Init(&closingClients);
    alpc_lifetime_mark_closing(&context->lifetime);
    /* alpc_state_claim_close prevents a second close from touching the same
     * native handles while this thread drains workers. */

    /* Disconnect first so Native waits on the connection port wake up.  Keep
     * the handle open until the context rundown drains those calls; closing a
     * handle concurrently with an in-flight Native wait is unsafe. */
    if (context->connectionPortHandle) {
        if (context->api.pfnNtAlpcDisconnectPort) {
            context->api.pfnNtAlpcDisconnectPort(context->connectionPortHandle, 0);
        }
    }

    /* Existing communication-port receives also hold the server rundown.
     * Detach their records and disconnect the handles first, but defer both
     * NtClose and record destruction until the server rundown has drained.
     * This keeps an onPostConnect/onRequest callback's record alive while a
     * concurrent close wakes the corresponding Native wait. */
    for (;;) {
        alpc_lock_acquire(&context->lockWord);
        client = (PALPC_PORT_SERVER_CLIENT)List_Head(&context->clientList);
        if (client && List_Remove(&context->clientList, &client->listEntry)) {
            if (context->clientCount) {
                --context->clientCount;
            }
        } else {
            /* A corrupted intrusive list must not turn shutdown into an
             * infinite loop.  There is no safe handle to recover here. */
            if (client) {
                List_Clear(&context->clientList);
            }
            context->clientCount = 0;
            client = NULL;
        }
        alpc_lock_release(&context->lockWord);
        if (!client) {
            break;
        }
        /* Pending records own client lifetime pins.  Release unclaimed
         * records before waiting for the client and cancel claimed records so
         * their active ServerReply calls can drain the server rundown. */
        alpc_cancel_pending_for_client(context, client, 0);
        /* Disconnect wakes a worker blocked in receive.  Wait for its
         * lifetime pin before closing the handle so NtClose cannot race an
         * in-flight Native call. */
        alpc_lifetime_mark_closing(&client->lifetime);
        if (client->portHandle && context->api.pfnNtAlpcDisconnectPort) {
            (void)context->api.pfnNtAlpcDisconnectPort(client->portHandle, 0);
        }
        if (!List_Insert_After(&closingClients,
                               List_Tail(&closingClients),
                               &client->listEntry)) {
            /* Keep the detached record reachable even if bookkeeping fails;
             * the private chain is drained after server rundown. */
            client->listEntry.next = NULL;
            client->listEntry.prev = NULL;
            client->listEntry.next = (LIST_ELEM *)orphanClients;
            orphanClients = client;
        }
    }

    /* All operations that entered before initialized was cleared now either
     * observed the disconnect or have returned.  Only after this point may a
     * client record or its native handle be destroyed. */
    alpc_lifetime_wait(&context->lifetime);
    /* No callback or ServerReply claimant remains after server rundown.  A
     * force pass covers a request captured by an operation that raced the
     * first detach loop. */
    alpc_cancel_all_pending(context, 1);
    alpc_snapshot_events(context, &closeEvents);
    if (context->connectionPortHandle && context->api.pfnNtClose) {
        (void)context->api.pfnNtClose(context->connectionPortHandle);
    }
    context->connectionPortHandle = NULL;

    /* A connection request that had already returned from the kernel when
     * shutdown started may have published one final client.  No server
     * operation remains after rundown, so it is now safe to drain that list. */
    for (;;) {
        alpc_lock_acquire(&context->lockWord);
        client = (PALPC_PORT_SERVER_CLIENT)List_Head(&context->clientList);
        if (client && List_Remove(&context->clientList, &client->listEntry)) {
            if (context->clientCount) {
                --context->clientCount;
            }
        } else {
            client = NULL;
            context->clientCount = 0;
        }
        alpc_lock_release(&context->lockWord);
        if (!client) {
            break;
        }
        alpc_lifetime_mark_closing(&client->lifetime);
        if (client->portHandle && context->api.pfnNtAlpcDisconnectPort) {
            (void)context->api.pfnNtAlpcDisconnectPort(client->portHandle, 0);
        }
        alpc_lifetime_wait(&client->lifetime);
        if (closeEvents.onClose) {
            closeEvents.onClose(client->portHandle, &client->clientId,
                                closeEvents.callbackContext);
        }
        if (client->portHandle && context->api.pfnNtClose) {
            (void)context->api.pfnNtClose(client->portHandle);
        }
        client->portHandle = NULL;
        alpc_release_client_info(client);
    }

    while ((client = (PALPC_PORT_SERVER_CLIENT)List_Head(&closingClients)) !=
           NULL) {
        (void)List_Remove(&closingClients, &client->listEntry);
        alpc_lifetime_wait(&client->lifetime);
        if (closeEvents.onClose) {
            closeEvents.onClose(client->portHandle, &client->clientId,
                                closeEvents.callbackContext);
        }
        if (client->portHandle && context->api.pfnNtClose) {
            (void)context->api.pfnNtClose(client->portHandle);
        }
        client->portHandle = NULL;
        alpc_release_client_info(client);
    }
    while (orphanClients != NULL) {
        client = orphanClients;
        orphanClients = (PALPC_PORT_SERVER_CLIENT)client->listEntry.next;
        alpc_lifetime_wait(&client->lifetime);
        if (closeEvents.onClose) {
            closeEvents.onClose(client->portHandle, &client->clientId,
                                closeEvents.callbackContext);
        }
        if (client->portHandle && context->api.pfnNtClose) {
            (void)context->api.pfnNtClose(client->portHandle);
        }
        client->portHandle = NULL;
        alpc_release_client_info(client);
    }
    if (context->api.pfnRtlFreeUnicodeString && context->unicodeName.Buffer) {
        context->api.pfnRtlFreeUnicodeString(&context->unicodeName);
    }
    alpc_lock_destroy(&context->lockWord);
    alpc_lock_destroy(&context->connectionReceiveLock);
    alpc_lifetime_destroy(&context->lifetime);
    alpc_reset_server_context(context);
}

uint8_t AlpcPort_Register_ServerEvtCallback(PALPC_PORT_SERVER_CONTEXT context,
                                             const ALPC_PORT_SERVER_EVENTS *events)
{
    if (!context || !events || alpc_state_load(&context->initialized) == 0 ||
        !alpc_lifetime_acquire(&context->lifetime)) {
        return 0;
    }
    alpc_lock_acquire(&context->lockWord);
    context->events = *events;
    alpc_lock_release(&context->lockWord);
    alpc_lifetime_release(&context->lifetime);
    return 1;
}

/* Dispatch an already dequeued application frame.  This is shared by the
 * accepted-port worker and the connection-port dispatcher, so both paths use
 * identical validation and reply semantics. */
static NTSTATUS alpc_dispatch_server_frame(PALPC_PORT_SERVER_CONTEXT context,
                                            PALPC_PORT_SERVER_CLIENT client,
                                            PALPC_PORT_FRAME frame,
                                            SIZE_T bufferLength,
                                            uint8_t *transportDisconnected)
{
    ALPC_PORT_SERVER_EVENTS events = ALPC_PORT_ZERO_INIT;
    NTSTATUS callbackStatus = STATUS_SUCCESS;
    NTSTATUS sendStatus = STATUS_UNSUCCESSFUL;
    LARGE_INTEGER noWait = ALPC_PORT_ZERO_INIT;
    ULONG replyLength = 0U;
    USHORT type = 0U;
    if (!context || !client || !frame || !transportDisconnected) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    *transportDisconnected = 0U;
    /* Native control notifications contain a PORT_MESSAGE rather than an SDK
     * frame.  Consume them before inspecting fields beyond the native header. */
    if (bufferLength >= (SIZE_T)sizeof(ALPC_PORT_MESSAGE)) {
        type = frame->header.u2.s2.Type;
        if (alpc_native_control_type(type)) {
            if (alpc_message_type_is(type,
                                     ALPC_PORT_MESSAGE_TYPE_PORT_CLOSED)) {
                *transportDisconnected = 1U;
                return STATUS_PORT_DISCONNECTED;
            }
            return STATUS_SUCCESS;
        }
    }
    if (!alpc_received_frame_valid(frame, bufferLength,
                                   context->maxMessageLength)) {
        sendStatus = alpc_send_error_reply(context, client->portHandle, frame,
                                           STATUS_DATA_ERROR,
                                           transportDisconnected);
        return sendStatus;
    }
    type = frame->header.u2.s2.Type;
    alpc_snapshot_events(context, &events);
    if (alpc_message_type_is(type, ALPC_PORT_MESSAGE_TYPE_DATAGRAM) ||
        (alpc_message_type_is(type, ALPC_PORT_MESSAGE_TYPE_REQUEST) &&
         (frame->Flags & ALPC_PORT_SEND_FLAG_ASYNC) != 0U)) {
        if (frame->Flags != ALPC_PORT_SEND_FLAG_ASYNC ||
            frame->Status != STATUS_SUCCESS) {
            return STATUS_DATA_ERROR;
        }
        if (events.onAsyncRequest) {
            callbackStatus = events.onAsyncRequest(
                client->portHandle, &frame->header.ClientId,
                frame->ControlId, frame->data, frame->PayloadLength,
                events.callbackContext);
        }
        return callbackStatus;
    }
    if (!alpc_message_type_is(type, ALPC_PORT_MESSAGE_TYPE_REQUEST) ||
        frame->Flags != 0U || frame->Status != STATUS_SUCCESS) {
        sendStatus = alpc_send_error_reply(context, client->portHandle, frame,
                                           STATUS_DATA_ERROR,
                                           transportDisconnected);
        return sendStatus;
    }
    replyLength = frame->PayloadLength;
    if (events.onSyncRequest) {
        NTSTATUS callbackResult = STATUS_SUCCESS;
        client->currentRequest = frame->header;
        client->currentControlId = frame->ControlId;
        client->currentPendingReply = NULL;
        client->currentRequestValid = 1U;
        client->currentRequestCaptured = 0U;
        callbackResult = events.onSyncRequest(
            client->portHandle, &frame->header.ClientId, frame->ControlId,
            frame->data, &replyLength,
            (ULONG)(context->maxMessageLength -
                    (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET),
            events.callbackContext);
        callbackStatus = alpc_finish_sync_callback(context, client,
                                                   callbackResult);
        if (callbackResult == STATUS_PENDING &&
            callbackStatus != STATUS_PENDING) {
            replyLength = 0U;
        }
    }
    if (callbackStatus == STATUS_PENDING) {
        return STATUS_PENDING;
    }
    if (!alpc_set_frame_length(frame, context->maxMessageLength, replyLength)) {
        sendStatus = alpc_send_error_reply(context, client->portHandle, frame,
                                           STATUS_INFO_LENGTH_MISMATCH,
                                           transportDisconnected);
        return sendStatus;
    }
    frame->header.u2.ZeroInit = 0U;
    frame->Flags = 0U;
    frame->Status = callbackStatus;
    sendStatus = context->api.pfnNtAlpcSendWaitReceivePort(
        client->portHandle, ALPC_PORT_SEND_FLAG_REPLY_MESSAGE, &frame->header,
        NULL, NULL, NULL, NULL, &noWait);
    if (sendStatus == STATUS_TIMEOUT || !NT_SUCCESS(sendStatus)) {
        *transportDisconnected = 1U;
    }
    return (sendStatus == STATUS_TIMEOUT || !NT_SUCCESS(sendStatus))
               ? sendStatus : callbackStatus;
}

NTSTATUS AlpcPort_ProcessBlockedEventEx(PALPC_PORT_SERVER_CONTEXT context,
                                         const LARGE_INTEGER *timeout)
{
    PALPC_PORT_FRAME frame = NULL;
    ALPC_PORT_SERVER_EVENTS events = ALPC_PORT_ZERO_INIT;
    SIZE_T bufferLength = 0;
    uint32_t responseControlId = 0;
    uint8_t deny = 0;
    LARGE_INTEGER timeoutValue = ALPC_PORT_ZERO_INIT;
    PLARGE_INTEGER timeoutArgument = NULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    PALPC_PORT_SERVER_CLIENT routedClient = NULL;
    ALPC_PORT_CONTEXT_ATTRIBUTES messageAttributes = ALPC_PORT_ZERO_INIT;
    uint8_t havePortContext = 0U;
    uint8_t nativeControl = 0U;
    uint8_t transportDisconnected = 0U;

    if (!context || alpc_state_load(&context->initialized) == 0 ||
        !context->connectionPortHandle ||
        !alpc_lifetime_acquire(&context->lifetime)) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    frame = (PALPC_PORT_FRAME)alpc_alloc(context->maxMessageLength);
    if (!frame) {
        alpc_lifetime_release(&context->lifetime);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    libc_memset(frame, 0, (size_t)context->maxMessageLength);
    libc_memset(&messageAttributes, 0, sizeof(messageAttributes));
    messageAttributes.Header.AllocatedAttributes =
        ALPC_PORT_MESSAGE_CONTEXT_ATTRIBUTE;
    bufferLength = context->maxMessageLength;
    if (timeout) {
        timeoutValue = *timeout;
        timeoutArgument = &timeoutValue;
    }
    alpc_lock_acquire(&context->connectionReceiveLock);
    status = context->api.pfnNtAlpcSendWaitReceivePort(
        context->connectionPortHandle, 0, NULL, NULL, &frame->header,
        &bufferLength, &messageAttributes.Header, timeoutArgument);
    alpc_lock_release(&context->connectionReceiveLock);
    if (status == STATUS_TIMEOUT || !NT_SUCCESS(status)) {
        alpc_free(frame);
        alpc_lifetime_release(&context->lifetime);
        return status;
    }
    if (bufferLength < (SIZE_T)sizeof(ALPC_PORT_MESSAGE)) {
        alpc_free(frame);
        alpc_lifetime_release(&context->lifetime);
        return STATUS_DATA_ERROR;
    }
    if (!alpc_message_type_is(frame->header.u2.s2.Type,
                              ALPC_PORT_MESSAGE_TYPE_CONNECTION_REQUEST)) {
        nativeControl = (uint8_t)alpc_native_control_type(
            frame->header.u2.s2.Type);
        /* Connection-complete/cancel notifications carry no SDK payload and
         * may trail the PORT_CLOSED notification for an endpoint that has
         * already been detached.  Consume them before resolving PortContext;
         * only PORT_CLOSED needs an endpoint lookup to drive onClose. */
        if (nativeControl &&
            !alpc_message_type_is(frame->header.u2.s2.Type,
                                  ALPC_PORT_MESSAGE_TYPE_PORT_CLOSED)) {
            alpc_free(frame);
            alpc_lifetime_release(&context->lifetime);
            return STATUS_SUCCESS;
        }
        havePortContext = (uint8_t)(
            (messageAttributes.Header.ValidAttributes &
             ALPC_PORT_MESSAGE_CONTEXT_ATTRIBUTE) != 0U);
        if (havePortContext) {
            if (!messageAttributes.Context.PortContext) {
                alpc_free(frame);
                alpc_lifetime_release(&context->lifetime);
                return STATUS_DATA_ERROR;
            }
            routedClient = alpc_acquire_client_by_context(
                context, messageAttributes.Context.PortContext);
            if (!routedClient ||
                (!nativeControl &&
                 (routedClient->clientId.UniqueProcess !=
                      frame->header.ClientId.UniqueProcess ||
                  (messageAttributes.Context.MessageId != 0U &&
                   messageAttributes.Context.MessageId !=
                       frame->header.MessageId)))) {
                if (routedClient) {
                    alpc_lifetime_release(&routedClient->lifetime);
                }
                alpc_free(frame);
                alpc_lifetime_release(&context->lifetime);
                return STATUS_DATA_ERROR;
            }
            if (alpc_state_load(&context->nativePortContextState) == 0) {
                alpc_state_store(&context->nativePortContextState, 1);
            }
        } else {
            /* Some providers and older test doubles do not return native
             * attributes.  Retain ClientId routing only for that case. */
            alpc_state_store(&context->nativePortContextState, -1);
            routedClient = alpc_acquire_client_by_id(
                context, &frame->header.ClientId);
        }
        if (!routedClient) {
            alpc_free(frame);
            alpc_lifetime_release(&context->lifetime);
            return STATUS_PORT_DISCONNECTED;
        }
        alpc_lock_acquire(&routedClient->receiveLock);
        status = alpc_dispatch_server_frame(context, routedClient, frame,
                                            bufferLength,
                                            &transportDisconnected);
        alpc_lock_release(&routedClient->receiveLock);
        if (transportDisconnected) {
            (void)alpc_drop_acquired_client(context, routedClient, 1U, 1U);
        } else {
            alpc_lifetime_release(&routedClient->lifetime);
        }
        alpc_free(frame);
        alpc_lifetime_release(&context->lifetime);
        return status;
    }
    if (!alpc_received_frame_valid(frame, bufferLength,
                                   context->maxMessageLength) ||
        !alpc_message_type_is(frame->header.u2.s2.Type,
                               ALPC_PORT_MESSAGE_TYPE_CONNECTION_REQUEST)) {
        if (alpc_message_type_is(frame->header.u2.s2.Type,
                                 ALPC_PORT_MESSAGE_TYPE_CONNECTION_REQUEST)) {
            status = alpc_reject_connection(context, frame);
            alpc_free(frame);
            alpc_lifetime_release(&context->lifetime);
            return NT_SUCCESS(status) ? STATUS_DATA_ERROR : status;
        }
        alpc_free(frame);
        alpc_lifetime_release(&context->lifetime);
        return STATUS_DATA_ERROR;
    }
    /* Connection frames carry only the control id.  Reject a peer that tries
     * to smuggle application payload, async flags, or a non-success status in
     * the handshake instead of accepting a partially initialized endpoint. */
    if (frame->Flags != 0 || frame->Status != STATUS_SUCCESS ||
        frame->PayloadLength != 0) {
        status = alpc_reject_connection(context, frame);
        alpc_free(frame);
        alpc_lifetime_release(&context->lifetime);
        return NT_SUCCESS(status) ? STATUS_DATA_ERROR : status;
    }
    alpc_snapshot_events(context, &events);
    responseControlId = frame->ControlId;
    deny = 1;
    if (events.onPreConnect) {
        events.onPreConnect(&responseControlId, &deny, events.callbackContext);
    }
    /* An accepted endpoint must be handed to the application so it can be
     * serviced or explicitly disconnected; otherwise reject it safely. */
    if (!events.onPostConnect) {
        deny = 1;
    }
    frame->ControlId = responseControlId;
    {
        HANDLE clientPort = NULL;
        ALPC_PORT_CLIENT_ID clientId = ALPC_PORT_ZERO_INIT;
        PALPC_PORT_SERVER_CLIENT pending = NULL;

        clientId = frame->header.ClientId;

        /* Allocate both the record and its never-reused pointer-sized token
         * before accept so Native never retains a freeable record address. */
        if (!deny) {
            pending = (PALPC_PORT_SERVER_CLIENT)alpc_alloc(sizeof(*pending));
            if (!pending) {
                (void)alpc_reject_connection(context, frame);
                alpc_free(frame);
                alpc_lifetime_release(&context->lifetime);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            libc_memset(pending, 0, sizeof(*pending));
            if (!alpc_lifetime_init(&pending->lifetime) ||
                !alpc_lock_init(&pending->receiveLock)) {
                (void)alpc_reject_connection(context, frame);
                alpc_release_client_info(pending);
                alpc_free(frame);
                alpc_lifetime_release(&context->lifetime);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            status = alpc_next_native_port_context_token(
                &pending->portContextToken);
            if (!NT_SUCCESS(status)) {
                (void)alpc_reject_connection(context, frame);
                alpc_release_client_info(pending);
                alpc_free(frame);
                alpc_lifetime_release(&context->lifetime);
                return status;
            }
        }
        status = context->api.pfnNtAlpcAcceptConnectPort(
            &clientPort, context->connectionPortHandle, 0, NULL,
            &context->portAttributes,
            pending ? pending->portContextToken : NULL,
            &frame->header, NULL, (BOOLEAN)!deny);
        if (!NT_SUCCESS(status)) {
            if (pending) {
                alpc_release_client_info(pending);
            }
            alpc_free(frame);
            alpc_lifetime_release(&context->lifetime);
            return status;
        }
        if (deny) {
            if (clientPort) {
                alpc_close_client_handle(context, clientPort, 1);
            }
            alpc_free(frame);
            alpc_lifetime_release(&context->lifetime);
            return STATUS_SUCCESS;
        }
        if (!clientPort) {
            alpc_release_client_info(pending);
            alpc_free(frame);
            alpc_lifetime_release(&context->lifetime);
            return STATUS_DATA_ERROR;
        }
        /* Unlike legacy LPC, NtAlpcAcceptConnectPort completes an accepted
         * ALPC connection itself.  Calling the legacy
         * NtCompleteConnectPort syscall here injects an extra native control
         * message and can corrupt the first application exchange. */
        /* The native API may return a communication handle even when a test
         * double or a future allocation path did not provide PortContext.
         * Never dereference a missing record; close the endpoint and reject
         * the connection instead of turning a malformed handshake into a
         * kernel/user-mode crash. */
        if (pending == (PALPC_PORT_SERVER_CLIENT)0) {
            alpc_close_client_handle(context, clientPort, 1);
            alpc_free(frame);
            alpc_lifetime_release(&context->lifetime);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        pending->portHandle = clientPort;
        pending->clientId = clientId;
        pending->controlId = responseControlId;
        if (!alpc_add_client(context, pending)) {
            alpc_close_client_handle(context, clientPort, 1);
            /* alpc_add_client destroys initialized synchronization fields on
             * failure, so only release the allocation here. */
            alpc_free(pending);
            alpc_free(frame);
            alpc_lifetime_release(&context->lifetime);
            return STATUS_DEVICE_BUSY;
        }
        if (events.onPostConnect) {
            events.onPostConnect(clientPort, &pending->clientId, responseControlId,
                                 events.callbackContext);
        }
        alpc_lifetime_release(&pending->lifetime);
    }
    alpc_free(frame);
    alpc_lifetime_release(&context->lifetime);
    return STATUS_SUCCESS;
}

NTSTATUS AlpcPort_ProcessBlockedEvent(PALPC_PORT_SERVER_CONTEXT context)
{
    return AlpcPort_ProcessBlockedEventEx(context, NULL);
}

static NTSTATUS alpc_drop_client(PALPC_PORT_SERVER_CONTEXT context,
                                  HANDLE clientPort,
                                  uint8_t disconnect,
                                  uint8_t notify)
{
    PALPC_PORT_SERVER_CLIENT client = NULL;
    ALPC_PORT_SERVER_EVENTS events = ALPC_PORT_ZERO_INIT;

    if (!context || alpc_state_load(&context->initialized) == 0 || !clientPort) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    client = alpc_detach_client(context, clientPort);
    if (!client) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    alpc_cancel_pending_for_client(context, client, 0);
    /* NtAlpcDisconnectPort wakes a worker blocked in receive.  Keep the
     * handle open until its lifetime pin is drained so the close callback can
     * still identify the endpoint with the original handle value. */
    alpc_lifetime_mark_closing(&client->lifetime);
    if (disconnect && context->api.pfnNtAlpcDisconnectPort) {
        (void)context->api.pfnNtAlpcDisconnectPort(client->portHandle, 0);
    }
    alpc_lifetime_wait(&client->lifetime);
    alpc_snapshot_events(context, &events);
    if (notify && events.onClose) {
        events.onClose(client->portHandle, &client->clientId, events.callbackContext);
    }
    alpc_close_client_handle(context, client->portHandle, 0);
    alpc_release_client_info(client);
    return STATUS_SUCCESS;
}

/* Removes a client for which the caller already owns one lifetime pin.  The
 * pointer identity check avoids dropping a newly accepted endpoint if Windows
 * reuses the closed endpoint's HANDLE during a concurrent disconnect. */
static NTSTATUS alpc_drop_acquired_client(
    PALPC_PORT_SERVER_CONTEXT context,
    PALPC_PORT_SERVER_CLIENT client,
    uint8_t disconnect,
    uint8_t notify)
{
    ALPC_PORT_SERVER_EVENTS events = ALPC_PORT_ZERO_INIT;
    uint8_t detached = 0U;

    if (!context || !client) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    alpc_lock_acquire(&context->lockWord);
    if (List_Contains(&context->clientList, &client->listEntry) &&
        List_Remove(&context->clientList, &client->listEntry)) {
        if (context->clientCount) {
            --context->clientCount;
        }
        detached = 1U;
    }
    alpc_lock_release(&context->lockWord);
    if (!detached) {
        alpc_lifetime_release(&client->lifetime);
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }

    alpc_cancel_pending_for_client(context, client, 0);
    alpc_lifetime_mark_closing(&client->lifetime);
    if (disconnect && context->api.pfnNtAlpcDisconnectPort) {
        (void)context->api.pfnNtAlpcDisconnectPort(client->portHandle, 0);
    }
    /* Drop the dispatcher's pin before waiting for all other users. */
    alpc_lifetime_release(&client->lifetime);
    alpc_lifetime_wait(&client->lifetime);
    alpc_snapshot_events(context, &events);
    if (notify && events.onClose) {
        events.onClose(client->portHandle, &client->clientId,
                       events.callbackContext);
    }
    alpc_close_client_handle(context, client->portHandle, 0);
    alpc_release_client_info(client);
    return STATUS_SUCCESS;
}

NTSTATUS AlpcPort_ProcessClientEventEx(PALPC_PORT_SERVER_CONTEXT context,
                                        HANDLE clientPort,
                                        const LARGE_INTEGER *timeout)
{
    PALPC_PORT_FRAME frame = NULL;
    PALPC_PORT_SERVER_CLIENT client = NULL;
    ALPC_PORT_SERVER_EVENTS events = ALPC_PORT_ZERO_INIT;
    SIZE_T bufferLength = 0;
    LARGE_INTEGER timeoutValue = ALPC_PORT_ZERO_INIT;
    PLARGE_INTEGER timeoutArgument = NULL;
    ULONG messageType = 0;
    ULONG replyLength = 0;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    NTSTATUS callbackStatus = STATUS_SUCCESS;
    NTSTATUS sendStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS result = STATUS_UNSUCCESSFUL;
    uint8_t dropClient = 0;
    HANDLE receivePort = clientPort;
    HANDLE replyPort = clientPort;
    LARGE_INTEGER noWait = ALPC_PORT_ZERO_INIT;

    if (!context || alpc_state_load(&context->initialized) == 0 || !clientPort ||
        !alpc_lifetime_acquire(&context->lifetime)) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    /* This compatibility receive path has no connection-port context
     * attribute, so upper layers must not advertise native routing. */
    alpc_state_store(&context->nativePortContextState, -1);
    client = alpc_acquire_client(context, clientPort);
    if (!client) {
        alpc_lifetime_release(&context->lifetime);
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    /* One receive worker per communication port is required by ALPC.  The
     * lock also keeps a disconnect from closing the handle mid-call. */
    alpc_lock_acquire(&client->receiveLock);
    replyPort = client->portHandle;
    frame = (PALPC_PORT_FRAME)alpc_alloc(context->maxMessageLength);
    if (!frame) {
        result = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    libc_memset(frame, 0, (size_t)context->maxMessageLength);
    bufferLength = context->maxMessageLength;
    if (timeout) {
        timeoutValue = *timeout;
        timeoutArgument = &timeoutValue;
    }
    status = context->api.pfnNtAlpcSendWaitReceivePort(
        receivePort, 0, NULL, NULL, &frame->header, &bufferLength, NULL,
        timeoutArgument);
    if (status == STATUS_PORT_DISCONNECTED) {
        alpc_free(frame);
        dropClient = 1;
        result = status;
        goto done;
    }
    if (status == STATUS_TIMEOUT || !NT_SUCCESS(status)) {
        alpc_free(frame);
        result = status;
        goto done;
    }
    if (!alpc_received_frame_valid(frame, bufferLength,
                                   context->maxMessageLength)) {
        /* Native disconnect/control notifications carry only a PORT_MESSAGE
         * header, not an SDK frame prefix.  Consume them before frame
         * validation so a normal client shutdown is not reported as corrupt
         * application data. */
        if (alpc_native_control_type(frame->header.u2.s2.Type)) {
            if (alpc_message_type_is(frame->header.u2.s2.Type,
                                     ALPC_PORT_MESSAGE_TYPE_PORT_CLOSED)) {
                dropClient = 1;
            }
            alpc_free(frame);
            result = STATUS_SUCCESS;
            goto done;
        }
        /* A native header is enough to preserve MessageId/ClientId for an
         * error reply even when the SDK frame prefix or payload is truncated.
         * Datagram traffic is intentionally fire-and-forget and is never
         * replied to. */
        if (bufferLength >= (SIZE_T)sizeof(ALPC_PORT_MESSAGE) &&
            bufferLength <= context->maxMessageLength &&
            alpc_message_type_is(frame->header.u2.s2.Type,
                                 ALPC_PORT_MESSAGE_TYPE_REQUEST) &&
            !(bufferLength >= (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET &&
              (frame->Flags & ALPC_PORT_SEND_FLAG_ASYNC))) {
            result = alpc_send_error_reply(context, replyPort, frame,
                                            STATUS_DATA_ERROR, &dropClient);
        } else {
            result = STATUS_DATA_ERROR;
        }
        alpc_free(frame);
        goto done;
    }
    messageType = frame->header.u2.s2.Type;
    if (alpc_native_control_type((USHORT)messageType) &&
        alpc_message_type_is((USHORT)messageType,
                             ALPC_PORT_MESSAGE_TYPE_PORT_CLOSED)) {
        alpc_free(frame);
        dropClient = 1;
        result = STATUS_SUCCESS;
        goto done;
    }
    if (alpc_message_type_is((USHORT)messageType,
                             ALPC_PORT_MESSAGE_TYPE_DATAGRAM) ||
        (alpc_message_type_is((USHORT)messageType,
                              ALPC_PORT_MESSAGE_TYPE_REQUEST) &&
         (frame->Flags & ALPC_PORT_SEND_FLAG_ASYNC))) {
        /* The SDK reserves all flags except ASYNC.  Invalid datagrams are
         * dropped (there is no reply channel); malformed async requests are
         * treated the same way. */
        if (frame->Flags != ALPC_PORT_SEND_FLAG_ASYNC ||
            frame->Status != STATUS_SUCCESS) {
            alpc_free(frame);
            result = STATUS_DATA_ERROR;
            goto done;
        }
        alpc_snapshot_events(context, &events);
        callbackStatus = STATUS_SUCCESS;
        if (events.onAsyncRequest) {
            callbackStatus = events.onAsyncRequest(
                replyPort, &frame->header.ClientId, frame->ControlId, frame->data,
                frame->PayloadLength, events.callbackContext);
        }
        alpc_free(frame);
        result = callbackStatus;
        goto done;
    }
    /* Windows emits a connection-complete notification (native type 11,
     * LPC_CONNECTION_REPLY) on the accepted communication endpoint.  It is
     * an internal handshake marker, not an application request; consuming it
     * without replying prevents it from being mistaken for the first request
     * made by the client. */
    if (alpc_native_control_type((USHORT)messageType) &&
        alpc_native_connection_complete_type((USHORT)messageType)) {
        alpc_free(frame);
        result = STATUS_SUCCESS;
        goto done;
    }
    if (!alpc_message_type_is((USHORT)messageType,
                              ALPC_PORT_MESSAGE_TYPE_REQUEST)) {
        /* A synchronous peer can otherwise wait forever on an unknown type.
         * ASYNC traffic has no reply channel and is intentionally dropped. */
        if ((frame->Flags & ALPC_PORT_SEND_FLAG_ASYNC) == 0) {
            result = alpc_send_error_reply(context, replyPort, frame,
                                           STATUS_DATA_ERROR, &dropClient);
        } else {
            result = STATUS_DATA_ERROR;
        }
        alpc_free(frame);
        goto done;
    }
    if (frame->Flags != 0 || frame->Status != STATUS_SUCCESS) {
        result = alpc_send_error_reply(context, replyPort, frame,
                                       STATUS_DATA_ERROR, &dropClient);
        alpc_free(frame);
        goto done;
    }
    alpc_snapshot_events(context, &events);
    replyLength = frame->PayloadLength;
    callbackStatus = STATUS_SUCCESS;
    if (events.onSyncRequest) {
        NTSTATUS callbackResult = STATUS_SUCCESS;
        client->currentRequest = frame->header;
        client->currentControlId = frame->ControlId;
        client->currentPendingReply = NULL;
        client->currentRequestValid = 1U;
        client->currentRequestCaptured = 0U;
        callbackResult = events.onSyncRequest(
            replyPort, &frame->header.ClientId, frame->ControlId, frame->data,
            &replyLength,
            (ULONG)(context->maxMessageLength - (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET),
            events.callbackContext);
        callbackStatus = alpc_finish_sync_callback(context, client,
                                                   callbackResult);
        if (callbackResult == STATUS_PENDING &&
            callbackStatus != STATUS_PENDING) {
            replyLength = 0U;
        }
    }
    if (callbackStatus == STATUS_PENDING) {
        alpc_free(frame);
        result = STATUS_PENDING;
        goto done;
    }
    if (!alpc_set_frame_length(frame, context->maxMessageLength, replyLength)) {
        result = alpc_send_error_reply(context, replyPort, frame,
                                       STATUS_INFO_LENGTH_MISMATCH,
                                       &dropClient);
        alpc_free(frame);
        goto done;
    }
    frame->header.u2.ZeroInit = 0U;
    frame->Flags = 0;
    frame->Status = callbackStatus;
    sendStatus = context->api.pfnNtAlpcSendWaitReceivePort(
        replyPort, ALPC_PORT_SEND_FLAG_REPLY_MESSAGE,
        &frame->header, NULL,
        NULL, NULL, NULL, &noWait);
    alpc_free(frame);
    if (sendStatus == STATUS_TIMEOUT || !NT_SUCCESS(sendStatus)) {
        dropClient = 1U;
        result = sendStatus;
        goto done;
    }
    result = callbackStatus;

done:
    alpc_lock_release(&client->receiveLock);
    alpc_lifetime_release(&client->lifetime);
    if (dropClient) {
        /* The client pin is released before detach so this path cannot wait
         * on itself.  The server context pin is still held. */
        (void)alpc_drop_client(context, replyPort, 1, 1);
    }
    alpc_lifetime_release(&context->lifetime);
    return result;
}

NTSTATUS AlpcPort_ProcessClientEvent(PALPC_PORT_SERVER_CONTEXT context,
                                      HANDLE clientPort)
{
    return AlpcPort_ProcessClientEventEx(context, clientPort, NULL);
}

NTSTATUS AlpcPort_ServerCaptureRequest(PALPC_PORT_SERVER_CONTEXT context,
                                       HANDLE clientPort,
                                       PALPC_PORT_REPLY_TOKEN token)
{
    PALPC_PORT_SERVER_CLIENT client = NULL;
    PALPC_PORT_PENDING_REPLY pending = NULL;
    uint64_t cookie = 0U;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    if (!context || !clientPort || !token) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    libc_memset(token, 0, sizeof(*token));
    if (alpc_state_load(&context->initialized) == 0 ||
        !alpc_lifetime_acquire(&context->lifetime)) {
        return ALPC_PORT_STATUS(STATUS_PORT_DISCONNECTED);
    }
    pending = (PALPC_PORT_PENDING_REPLY)alpc_alloc(sizeof(*pending));
    if (!pending) {
        alpc_lifetime_release(&context->lifetime);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    libc_memset(pending, 0, sizeof(*pending));
    client = alpc_acquire_client(context, clientPort);
    if (!client) {
        alpc_free(pending);
        alpc_lifetime_release(&context->lifetime);
        return ALPC_PORT_STATUS(STATUS_PORT_DISCONNECTED);
    }

    alpc_lock_acquire(&context->lockWord);
    if (alpc_state_load(&context->initialized) == 0 ||
        !List_Contains(&context->clientList, &client->listEntry)) {
        status = STATUS_PORT_DISCONNECTED;
    } else if (!client->currentRequestValid ||
               client->currentRequestCaptured ||
               client->currentPendingReply) {
        status = STATUS_INVALID_PARAMETER;
    } else {
        status = alpc_next_pending_reply_cookie(&cookie);
        if (status == STATUS_SUCCESS) {
            pending->client = client;
            pending->request = client->currentRequest;
            pending->controlId = client->currentControlId;
            pending->cookie = cookie;
            pending->state = ALPC_PENDING_REPLY_CAPTURED;
            if (!List_Insert_After(&context->pendingReplies,
                                   List_Tail(&context->pendingReplies),
                                   &pending->listEntry)) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                context->pendingReplyCookieSequence = cookie;
                client->currentPendingReply = pending;
                client->currentRequestCaptured = 1U;
                token->opaque = pending;
                token->cookie = cookie;
                status = STATUS_SUCCESS;
            }
        }
    }
    alpc_lock_release(&context->lockWord);

    if (status != STATUS_SUCCESS) {
        alpc_lifetime_release(&client->lifetime);
        alpc_free(pending);
    }
    /* On success the pending record owns the client pin until reply or
     * cancellation. */
    alpc_lifetime_release(&context->lifetime);
    return status;
}

NTSTATUS AlpcPort_ServerReply(PALPC_PORT_SERVER_CONTEXT context,
                              PALPC_PORT_REPLY_TOKEN token,
                              const void *payload,
                              ULONG payloadLength,
                              NTSTATUS requestStatus,
                              const LARGE_INTEGER *timeout)
{
    union {
        LONGLONG alignment;
        UCHAR bytes[ALPC_PORT_EMERGENCY_REPLY_SIZE];
    } emergencyFrame = ALPC_PORT_ZERO_INIT;
    PALPC_PORT_PENDING_REPLY pending = NULL;
    PALPC_PORT_PENDING_REPLY registered = NULL;
    PALPC_PORT_SERVER_CLIENT client = NULL;
    PALPC_PORT_FRAME frame = NULL;
    SIZE_T capacity = 0U;
    SIZE_T frameLength = 0U;
    LARGE_INTEGER timeoutValue = ALPC_PORT_ZERO_INIT;
    PLARGE_INTEGER timeoutArgument = NULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    uint8_t releasePending = 0U;
    uint8_t ready = 0U;
    uint8_t frameAllocated = 0U;

    if (!context || !token || !token->opaque || token->cookie == 0U ||
        (payloadLength != 0U && !payload) ||
        !context->api.pfnNtAlpcSendWaitReceivePort) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    if (alpc_state_load(&context->initialized) == 0 ||
        !alpc_lifetime_acquire(&context->lifetime)) {
        return ALPC_PORT_STATUS(STATUS_PORT_DISCONNECTED);
    }

    /* Claim by registry identity and cookie.  The token pointer is never
     * dereferenced, so a stale or forged token cannot address arbitrary
     * memory. */
    alpc_lock_acquire(&context->lockWord);
    pending = alpc_find_pending_locked(context, token);
    if (pending && pending->state == ALPC_PENDING_REPLY_CAPTURED) {
        pending->state = ALPC_PENDING_REPLY_CLAIMED_EARLY;
    } else if (pending && pending->state == ALPC_PENDING_REPLY_PENDING) {
        pending->state = ALPC_PENDING_REPLY_CLAIMED;
    } else {
        pending = NULL;
    }
    if (pending) {
        client = pending->client;
    }
    alpc_lock_release(&context->lockWord);
    if (!pending || !client) {
        alpc_lifetime_release(&context->lifetime);
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }

    /* If the callback is still running, this waits until it has committed
     * STATUS_PENDING or cancelled the early claim. */
    alpc_lock_acquire(&client->receiveLock);
    alpc_lock_acquire(&context->lockWord);
    registered = alpc_find_pending_locked(context, token);
    if (registered != pending ||
        pending->state == ALPC_PENDING_REPLY_CLAIMED_CANCELLED ||
        alpc_state_load(&context->initialized) == 0 ||
        !List_Contains(&context->clientList, &client->listEntry) ||
        !client->portHandle) {
        if (registered == pending &&
            List_Remove(&context->pendingReplies, &pending->listEntry)) {
            releasePending = 1U;
        }
        status = STATUS_PORT_DISCONNECTED;
    } else if (pending->state == ALPC_PENDING_REPLY_CLAIMED ||
               pending->state == ALPC_PENDING_REPLY_CLAIMED_READY) {
        ready = 1U;
    } else {
        pending->state = ALPC_PENDING_REPLY_CLAIMED_CANCELLED;
        if (List_Remove(&context->pendingReplies, &pending->listEntry)) {
            releasePending = 1U;
        }
        status = STATUS_INVALID_PARAMETER;
    }
    alpc_lock_release(&context->lockWord);
    if (!ready) {
        goto reply_done;
    }

    capacity = context->maxMessageLength;
    if ((SIZE_T)payloadLength > capacity - (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET) {
        status = STATUS_INFO_LENGTH_MISMATCH;
        goto pre_send_failure;
    }
    frameLength = (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET +
                  (SIZE_T)payloadLength;
    if (frameLength <= (SIZE_T)sizeof(emergencyFrame.bytes)) {
        frame = (PALPC_PORT_FRAME)(void *)emergencyFrame.bytes;
    } else {
        frame = (PALPC_PORT_FRAME)alpc_alloc(frameLength);
        if (!frame) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto pre_send_failure;
        }
        frameAllocated = 1U;
    }
    if (!alpc_init_frame(frame, frameLength, 0U,
                         pending->controlId, 0U, payload, payloadLength)) {
        status = STATUS_INFO_LENGTH_MISMATCH;
        goto pre_send_failure;
    }
    frame->header.ClientId = pending->request.ClientId;
    frame->header.MessageId = pending->request.MessageId;
    frame->header.u3 = pending->request.u3;
    frame->Status = requestStatus;
    if (timeout) {
        timeoutValue = *timeout;
        timeoutArgument = &timeoutValue;
    }

    /* Recheck cancellation immediately before the one native send attempt.
     * A disconnect after this point still cannot close the handle because the
     * pending record owns a client lifetime pin. */
    alpc_lock_acquire(&context->lockWord);
    registered = alpc_find_pending_locked(context, token);
    if (registered != pending ||
        pending->state == ALPC_PENDING_REPLY_CLAIMED_CANCELLED ||
        alpc_state_load(&context->initialized) == 0 ||
        !List_Contains(&context->clientList, &client->listEntry) ||
        !client->portHandle) {
        if (registered == pending &&
            List_Remove(&context->pendingReplies, &pending->listEntry)) {
            releasePending = 1U;
        }
        status = STATUS_PORT_DISCONNECTED;
    } else {
        ready = 2U;
    }
    alpc_lock_release(&context->lockWord);
    if (ready != 2U) {
        goto reply_done;
    }

    status = context->api.pfnNtAlpcSendWaitReceivePort(
        client->portHandle, ALPC_PORT_SEND_FLAG_REPLY_MESSAGE,
        &frame->header, NULL, NULL, NULL, NULL, timeoutArgument);

    /* A native send attempt consumes the request regardless of transport
     * success.  Retrying after an ambiguous send could produce two replies. */
    alpc_lock_acquire(&context->lockWord);
    registered = alpc_find_pending_locked(context, token);
    if (registered == pending &&
        List_Remove(&context->pendingReplies, &pending->listEntry)) {
        releasePending = 1U;
    }
    alpc_lock_release(&context->lockWord);
    token->opaque = NULL;
    token->cookie = 0U;
    goto reply_done;

pre_send_failure:
    /* Allocation and frame-construction failures happen before a native send,
     * so a live request remains retryable with the same token. */
    alpc_lock_acquire(&context->lockWord);
    registered = alpc_find_pending_locked(context, token);
    if (registered == pending &&
        pending->state != ALPC_PENDING_REPLY_CLAIMED_CANCELLED &&
        alpc_state_load(&context->initialized) != 0 &&
        List_Contains(&context->clientList, &client->listEntry) &&
        client->portHandle) {
        pending->state = ALPC_PENDING_REPLY_PENDING;
    } else if (registered == pending &&
               List_Remove(&context->pendingReplies,
                           &pending->listEntry)) {
        releasePending = 1U;
        token->opaque = NULL;
        token->cookie = 0U;
    }
    alpc_lock_release(&context->lockWord);

reply_done:
    if (frame && frameAllocated) {
        alpc_free(frame);
    }
    alpc_lock_release(&client->receiveLock);
    if (releasePending) {
        token->opaque = NULL;
        token->cookie = 0U;
        alpc_release_pending_record(pending);
    }
    alpc_lifetime_release(&context->lifetime);
    return status;
}

NTSTATUS AlpcPort_ServerDisconnectClient(PALPC_PORT_SERVER_CONTEXT context,
                                           HANDLE clientPort)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    if (!context || alpc_state_load(&context->initialized) == 0 ||
        !alpc_lifetime_acquire(&context->lifetime)) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    status = alpc_drop_client(context, clientPort, 1, 1);
    alpc_lifetime_release(&context->lifetime);
    return status;
}

NTSTATUS AlpcPort_ServerGetClientToken(PALPC_PORT_SERVER_CONTEXT context,
                                       HANDLE clientPort,
                                       PALPC_PORT_ENDPOINT_TOKEN token)
{
    PALPC_PORT_SERVER_CLIENT client = NULL;

    if (token) {
        token->opaque = NULL;
        token->cookie = 0U;
    }
    if (!context || !clientPort || !token ||
        alpc_state_load(&context->initialized) == 0 ||
        !alpc_lifetime_acquire(&context->lifetime)) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    alpc_lock_acquire(&context->lockWord);
    client = alpc_find_client_locked(context, clientPort);
    if (client && client->endpointCookie != 0U) {
        token->opaque = (PVOID)client;
        token->cookie = client->endpointCookie;
    }
    alpc_lock_release(&context->lockWord);
    alpc_lifetime_release(&context->lifetime);
    return token->opaque ? STATUS_SUCCESS : STATUS_PORT_DISCONNECTED;
}

NTSTATUS AlpcPort_ServerDisconnectClientToken(
    PALPC_PORT_SERVER_CONTEXT context,
    const ALPC_PORT_ENDPOINT_TOKEN *token)
{
    PALPC_PORT_SERVER_CLIENT current = NULL;
    PALPC_PORT_SERVER_CLIENT client = NULL;
    NTSTATUS status = STATUS_PORT_DISCONNECTED;

    if (!context || !token || !token->opaque || token->cookie == 0U ||
        alpc_state_load(&context->initialized) == 0 ||
        !alpc_lifetime_acquire(&context->lifetime)) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    alpc_lock_acquire(&context->lockWord);
    for (current = (PALPC_PORT_SERVER_CLIENT)List_Head(&context->clientList);
         current != NULL;
         current = (PALPC_PORT_SERVER_CLIENT)List_Next(&current->listEntry)) {
        if ((PVOID)current == token->opaque &&
            current->endpointCookie == token->cookie) {
            if (alpc_lifetime_acquire(&current->lifetime)) {
                client = current;
            }
            break;
        }
    }
    alpc_lock_release(&context->lockWord);
    if (client) {
        status = alpc_drop_acquired_client(context, client, 1U, 1U);
    }
    alpc_lifetime_release(&context->lifetime);
    return status;
}

uint8_t AlpcPort_ServerHasNativePortContext(
    const ALPC_PORT_SERVER_CONTEXT *context)
{
    return (uint8_t)(context &&
                     alpc_state_load(&context->initialized) != 0 &&
                     alpc_state_load(&context->nativePortContextState) == 1);
}

NTSTATUS AlpcPort_Connect(PALPC_PORT_CLIENT_CONFIG config,
                          PALPC_PORT_CLIENT_CONTEXT context)
{
    ALPC_PORT_SDK_ATTRIBUTES portAttributes = ALPC_PORT_ZERO_INIT;
    SECURITY_QUALITY_OF_SERVICE securityQos = ALPC_PORT_ZERO_INIT;
    PALPC_PORT_FRAME connectionFrame = NULL;
    SIZE_T bufferLength = 0;
    SIZE_T maxMessageLength = 0;
    LARGE_INTEGER connectDeadline = ALPC_PORT_ZERO_INIT;
    LARGE_INTEGER remainingTimeout = ALPC_PORT_ZERO_INIT;
    const LARGE_INTEGER *deadlineArgument = NULL;
    PLARGE_INTEGER timeoutArgument = NULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    if (!config || !context || alpc_state_load(&context->initialized) != 0 ||
        !alpc_name_valid(&config->portName) || !alpc_client_api_valid(&config->api)) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    maxMessageLength = alpc_normalize_max_message_length(config->maxMessageLength);
    if (!alpc_max_message_length_valid(maxMessageLength)) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (config->connectTimeout) {
        status = AlpcPort_NormalizeTimeout(
            config->api.pfnNtQuerySystemTime, config->connectTimeout,
            &connectDeadline);
        if (status != STATUS_SUCCESS) {
            return status;
        }
        deadlineArgument = &connectDeadline;
    }
    alpc_reset_client_context(context);
    context->api = config->api;
    if (!alpc_lifetime_init(&context->lifetime) ||
        !alpc_lock_init(&context->sendLock) ||
        !alpc_lock_init(&context->receiveLock)) {
        alpc_lock_destroy(&context->receiveLock);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_lock_destroy(&context->sendLock);
        alpc_reset_client_context(context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = alpc_build_unicode_name(context->api.pfnRtlInitAnsiString,
                                      context->api.pfnRtlAnsiStringToUnicodeString,
                                      &config->portName, &context->unicodeName);
    if (!NT_SUCCESS(status)) {
        alpc_lock_destroy(&context->receiveLock);
        alpc_lock_destroy(&context->sendLock);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_client_context(context);
        return status;
    }
    connectionFrame = (PALPC_PORT_FRAME)alpc_alloc(maxMessageLength);
    if (!connectionFrame) {
        context->api.pfnRtlFreeUnicodeString(&context->unicodeName);
        alpc_lock_destroy(&context->receiveLock);
        alpc_lock_destroy(&context->sendLock);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_client_context(context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!alpc_init_frame(connectionFrame, maxMessageLength,
                         (USHORT)ALPC_PORT_MESSAGE_TYPE_CONNECTION_REQUEST,
                         config->helloId, 0, NULL, 0)) {
        alpc_free(connectionFrame);
        context->api.pfnRtlFreeUnicodeString(&context->unicodeName);
        alpc_lock_destroy(&context->receiveLock);
        alpc_lock_destroy(&context->sendLock);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_client_context(context);
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    securityQos = config->securityQos;
    alpc_init_qos(&securityQos);
    alpc_init_port_attributes(&portAttributes, maxMessageLength,
                              config->portFlags |
                                  ALPC_PORT_FLAG_ALLOW_LPC_REQUESTS,
                              0, 0, 0, 0, &securityQos);
    bufferLength = maxMessageLength;
    status = alpc_refresh_timeout(context->api.pfnNtQuerySystemTime,
                                  deadlineArgument, &remainingTimeout,
                                  &timeoutArgument);
    if (status != STATUS_SUCCESS) {
        alpc_free(connectionFrame);
        context->api.pfnRtlFreeUnicodeString(&context->unicodeName);
        alpc_lock_destroy(&context->receiveLock);
        alpc_lock_destroy(&context->sendLock);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_client_context(context);
        return status;
    }
    status = context->api.pfnNtAlpcConnectPort(
        &context->portHandle, &context->unicodeName, NULL, &portAttributes, 0,
        NULL, &connectionFrame->header, &bufferLength, NULL, NULL,
        timeoutArgument);
    if (status == STATUS_TIMEOUT || !NT_SUCCESS(status) || !context->portHandle) {
        if (status != STATUS_TIMEOUT && NT_SUCCESS(status)) {
            status = STATUS_DATA_ERROR;
        }
        if (context->portHandle && context->api.pfnNtClose) {
            if (context->api.pfnNtAlpcDisconnectPort) {
                (void)context->api.pfnNtAlpcDisconnectPort(context->portHandle, 0);
            }
            context->api.pfnNtClose(context->portHandle);
        }
        alpc_free(connectionFrame);
        context->api.pfnRtlFreeUnicodeString(&context->unicodeName);
        alpc_lock_destroy(&context->receiveLock);
        alpc_lock_destroy(&context->sendLock);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_client_context(context);
        return status;
    }
    if (!alpc_received_frame_valid(connectionFrame, bufferLength,
                                   maxMessageLength) ||
        !alpc_message_type_is(connectionFrame->header.u2.s2.Type,
                               ALPC_PORT_MESSAGE_TYPE_CONNECTION_REQUEST) ||
        connectionFrame->Flags != 0 || connectionFrame->Status != STATUS_SUCCESS ||
        connectionFrame->PayloadLength != 0) {
        if (context->portHandle && context->api.pfnNtClose) {
            if (context->api.pfnNtAlpcDisconnectPort) {
                (void)context->api.pfnNtAlpcDisconnectPort(context->portHandle, 0);
            }
            context->api.pfnNtClose(context->portHandle);
        }
        alpc_free(connectionFrame);
        context->api.pfnRtlFreeUnicodeString(&context->unicodeName);
        alpc_lock_destroy(&context->receiveLock);
        alpc_lock_destroy(&context->sendLock);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_client_context(context);
        return STATUS_DATA_ERROR;
    }
    context->maxMessageLength = maxMessageLength;
    context->negotiatedControlId = connectionFrame->ControlId;
    context->onIncomingRequest = config->onIncomingRequest;
    context->incomingRequestContext = config->incomingRequestContext;
    /* NtAlpcConnectPort may return after queuing the request but before the
     * server accepts it.  The type-11 marker is the native completion of that
     * handshake.  Do not publish the client until it arrives: sending during
     * this window fails cross-process with STATUS_LPC_REQUESTS_NOT_ALLOWED. */
    libc_memset(connectionFrame, 0, (size_t)maxMessageLength);
    bufferLength = maxMessageLength;
    status = alpc_refresh_timeout(context->api.pfnNtQuerySystemTime,
                                  deadlineArgument, &remainingTimeout,
                                  &timeoutArgument);
    if (status != STATUS_SUCCESS) {
        if (context->api.pfnNtAlpcDisconnectPort) {
            (void)context->api.pfnNtAlpcDisconnectPort(context->portHandle, 0);
        }
        if (context->api.pfnNtClose) {
            (void)context->api.pfnNtClose(context->portHandle);
        }
        alpc_free(connectionFrame);
        context->api.pfnRtlFreeUnicodeString(&context->unicodeName);
        alpc_lock_destroy(&context->receiveLock);
        alpc_lock_destroy(&context->sendLock);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_client_context(context);
        return status;
    }
    status = context->api.pfnNtAlpcSendWaitReceivePort(
        context->portHandle, 0, NULL, NULL, &connectionFrame->header,
        &bufferLength, NULL, timeoutArgument);
    if (status != STATUS_SUCCESS ||
        bufferLength < (SIZE_T)sizeof(ALPC_PORT_MESSAGE) ||
        (SIZE_T)connectionFrame->header.u1.s1.TotalLength <
            (SIZE_T)sizeof(ALPC_PORT_MESSAGE) ||
        (SIZE_T)connectionFrame->header.u1.s1.TotalLength > bufferLength ||
        (SIZE_T)connectionFrame->header.u1.s1.TotalLength > maxMessageLength ||
        connectionFrame->header.u1.s1.DataLength !=
            connectionFrame->header.u1.s1.TotalLength -
                (USHORT)sizeof(ALPC_PORT_MESSAGE) ||
        !alpc_native_connection_complete_type(
            connectionFrame->header.u2.s2.Type)) {
        NTSTATUS markerStatus = status == STATUS_SUCCESS
                                    ? STATUS_DATA_ERROR : status;
        if (context->api.pfnNtAlpcDisconnectPort) {
            (void)context->api.pfnNtAlpcDisconnectPort(context->portHandle, 0);
        }
        if (context->api.pfnNtClose) {
            (void)context->api.pfnNtClose(context->portHandle);
        }
        alpc_free(connectionFrame);
        context->api.pfnRtlFreeUnicodeString(&context->unicodeName);
        alpc_lock_destroy(&context->receiveLock);
        alpc_lock_destroy(&context->sendLock);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_client_context(context);
        return markerStatus;
    }
    alpc_state_store(&context->initialized, 1);
    alpc_free(connectionFrame);
    return STATUS_SUCCESS;
}

NTSTATUS AlpcPort_SendMessage(PALPC_PORT_CLIENT_CONTEXT context,
                              const void *message,
                              ULONG messageLength,
                              ULONG controlId,
                              ULONG flags,
                              AlpcPort_SyncReplyCallback replyCallback,
                              PVOID callbackContext,
                              const LARGE_INTEGER *timeout)
{
    PALPC_PORT_FRAME requestFrame = NULL;
    PALPC_PORT_FRAME replyFrame = NULL;
    SIZE_T replyLength = 0;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    NTSTATUS callbackStatus = STATUS_SUCCESS;
    NTSTATUS remoteStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS result = STATUS_UNSUCCESSFUL;
    void *replyCopy = NULL;
    ULONG replyCopyLength = 0;
    uint8_t asynchronous = 0;
    uint8_t haveReply = 0;
    uint8_t sendLockHeld = 0;
    uint32_t receiveAttempts = 0;
    LARGE_INTEGER deadline = ALPC_PORT_ZERO_INIT;
    LARGE_INTEGER timeoutValue = ALPC_PORT_ZERO_INIT;
    const LARGE_INTEGER *deadlineArgument = NULL;
    PLARGE_INTEGER timeoutArgument = NULL;

    if (!context || alpc_state_load(&context->initialized) == 0 ||
        !context->portHandle ||
        (!message && messageLength) || (flags & ~ALPC_PORT_SEND_FLAG_ASYNC)) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    if (context->maxMessageLength < (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET ||
        (SIZE_T)messageLength > context->maxMessageLength -
                                (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    if (!alpc_lifetime_acquire(&context->lifetime)) {
        return STATUS_DEVICE_BUSY;
    }
    if (timeout) {
        status = AlpcPort_NormalizeTimeout(
            context->api.pfnNtQuerySystemTime, timeout, &deadline);
        if (status != STATUS_SUCCESS) {
            alpc_lifetime_release(&context->lifetime);
            return status;
        }
        deadlineArgument = &deadline;
    }
    asynchronous = (flags & ALPC_PORT_SEND_FLAG_ASYNC) ? 1U : 0U;
    /* Synchronous calls share a receive path and must remain serialized.
     * Datagram sends own their frames and have no reply state, so allowing
     * them to proceed independently prevents a receive callback from waiting
     * behind a synchronous request whose peer needs that callback to finish. */
    if (!asynchronous) {
        alpc_lock_acquire(&context->sendLock);
        sendLockHeld = 1U;
    }
    requestFrame = (PALPC_PORT_FRAME)alpc_alloc(context->maxMessageLength);
    if (!requestFrame) {
        result = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    if (!alpc_init_frame(requestFrame, context->maxMessageLength, 0U,
                         controlId, flags, message, messageLength)) {
        alpc_free(requestFrame);
        result = STATUS_INFO_LENGTH_MISMATCH;
        goto done;
    }
    if (asynchronous) {
        status = alpc_refresh_timeout(context->api.pfnNtQuerySystemTime,
                                      deadlineArgument, &timeoutValue,
                                      &timeoutArgument);
        if (status != STATUS_SUCCESS) {
            alpc_free(requestFrame);
            result = status;
            goto done;
        }
        status = context->api.pfnNtAlpcSendWaitReceivePort(
            context->portHandle, 0, &requestFrame->header, NULL, NULL, NULL,
            NULL, timeoutArgument);
        alpc_free(requestFrame);
        result = status;
        goto done;
    }
    replyFrame = (PALPC_PORT_FRAME)alpc_alloc(context->maxMessageLength);
    if (!replyFrame) {
        alpc_free(requestFrame);
        result = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    libc_memset(replyFrame, 0, (size_t)context->maxMessageLength);
    replyLength = context->maxMessageLength;
    /* Keep a native synchronous request pending while the server handles it.
     * Some builds return a local connection marker/request notification first;
     * drain those notifications with receive-only calls without resending the
     * application request. */
    replyLength = context->maxMessageLength;
    status = alpc_refresh_timeout(context->api.pfnNtQuerySystemTime,
                                  deadlineArgument, &timeoutValue,
                                  &timeoutArgument);
    if (status != STATUS_SUCCESS) {
        alpc_free(requestFrame);
        alpc_free(replyFrame);
        result = status;
        goto done;
    }
    status = context->api.pfnNtAlpcSendWaitReceivePort(
        context->portHandle, ALPC_PORT_NATIVE_FLAG_SYNC_REQUEST,
        &requestFrame->header, NULL, &replyFrame->header,
        &replyLength, NULL, timeoutArgument);
    if (status != STATUS_TIMEOUT && NT_SUCCESS(status)) {
        /* A connection-complete marker may be queued after Connect's
         * best-effort poll.  Drain only that native control message.  Real
         * Windows reports a synchronous reply as LPC_REQUEST, so Type cannot
         * distinguish it from an unsolicited request; the synchronous native
         * call itself performs reply correlation. */
        for (receiveAttempts = 0; receiveAttempts < 16U; ++receiveAttempts) {
            USHORT incomingType = replyFrame->header.u2.s2.Type;
            if (alpc_native_connection_complete_type(incomingType)) {
                libc_memset(replyFrame, 0, (size_t)context->maxMessageLength);
                replyLength = context->maxMessageLength;
                status = alpc_refresh_timeout(
                    context->api.pfnNtQuerySystemTime, deadlineArgument,
                    &timeoutValue, &timeoutArgument);
                if (status != STATUS_SUCCESS) {
                    break;
                }
                status = context->api.pfnNtAlpcSendWaitReceivePort(
                    context->portHandle, 0, NULL, NULL, &replyFrame->header,
                    &replyLength, NULL, timeoutArgument);
                if (status == STATUS_TIMEOUT || !NT_SUCCESS(status)) {
                    break;
                }
                continue;
            }
            break;
        }
        if (receiveAttempts == 16U && NT_SUCCESS(status)) {
            status = STATUS_DEVICE_BUSY;
        }
    }
    alpc_free(requestFrame);
    if (status == STATUS_TIMEOUT || !NT_SUCCESS(status)) {
        alpc_free(replyFrame);
        result = status;
        goto done;
    }
    if (!alpc_received_frame_valid(replyFrame, replyLength,
                                   context->maxMessageLength) ||
        replyFrame->ControlId != controlId ||
        replyFrame->Flags != 0) {
        alpc_free(replyFrame);
        result = STATUS_DATA_ERROR;
        goto done;
    }
    remoteStatus = replyFrame->Status;
    replyCopyLength = replyFrame->PayloadLength;
    if (replyCopyLength && replyCallback) {
        replyCopy = alpc_alloc(replyCopyLength);
        if (!replyCopy) {
            alpc_free(replyFrame);
            result = STATUS_INSUFFICIENT_RESOURCES;
            goto done;
        }
        libc_memcpy(replyCopy, replyFrame->data, replyCopyLength);
    }
    haveReply = 1;
    alpc_free(replyFrame);
    callbackStatus = STATUS_SUCCESS;
    /* STATUS_TIMEOUT is numerically non-negative in NTSTATUS, but it is a
     * terminal timeout for RPC and must not be folded into transport success. */
    result = (remoteStatus == STATUS_TIMEOUT || !NT_SUCCESS(remoteStatus))
                 ? remoteStatus : status;

done:
    if (sendLockHeld) {
        alpc_lock_release(&context->sendLock);
    }
    alpc_lifetime_release(&context->lifetime);
    if (haveReply && replyCallback) {
        callbackStatus = replyCallback((const uint8_t *)replyCopy,
                                       replyCopyLength, callbackContext);
        if (!NT_SUCCESS(callbackStatus)) {
            result = callbackStatus;
        }
    }
    alpc_free(replyCopy);
    return result;
}

uint8_t AlpcPort_ClientAcquire(PALPC_PORT_CLIENT_CONTEXT context)
{
    if (!context || alpc_state_load(&context->initialized) == 0) {
        return 0U;
    }
    return (uint8_t)(alpc_lifetime_acquire(&context->lifetime) ? 1U : 0U);
}

void AlpcPort_ClientRelease(PALPC_PORT_CLIENT_CONTEXT context)
{
    if (!context) {
        return;
    }
    alpc_lifetime_release(&context->lifetime);
}

void AlpcPort_ClientReceiveLock(PALPC_PORT_CLIENT_CONTEXT context)
{
    if (context) {
        alpc_lock_acquire(&context->receiveLock);
    }
}

void AlpcPort_ClientReceiveUnlock(PALPC_PORT_CLIENT_CONTEXT context)
{
    if (context) {
        alpc_lock_release(&context->receiveLock);
    }
}

void AlpcPort_DisConnect(PALPC_PORT_CLIENT_CONTEXT context)
{
    if (!context || !alpc_state_claim_close(&context->initialized)) {
        return;
    }
    /* alpc_state_claim_close already published the disconnected state. */
    /* Disconnecting the endpoint wakes a synchronous sender blocked inside
     * NtAlpcSendWaitReceivePort.  Keep the handle open until the sender leaves
     * its lifetime pin; closing it concurrently with a Native call is unsafe. */
    alpc_lifetime_mark_closing(&context->lifetime);
    if (context->portHandle) {
        if (context->api.pfnNtAlpcDisconnectPort) {
            context->api.pfnNtAlpcDisconnectPort(context->portHandle, 0);
        }
    }
    alpc_lifetime_wait(&context->lifetime);
    if (context->portHandle) {
        if (context->api.pfnNtClose) {
            context->api.pfnNtClose(context->portHandle);
        }
    }
    if (context->api.pfnRtlFreeUnicodeString && context->unicodeName.Buffer) {
        context->api.pfnRtlFreeUnicodeString(&context->unicodeName);
    }
    alpc_lock_destroy(&context->receiveLock);
    alpc_lock_destroy(&context->sendLock);
    alpc_lifetime_destroy(&context->lifetime);
    alpc_reset_client_context(context);
}

void AlpcPort_Disconnect(PALPC_PORT_CLIENT_CONTEXT context)
{
    AlpcPort_DisConnect(context);
}
