#include "lpc_port.h"
#include "def.h"

#ifndef _STDINT_H
#  define _STDINT_H
#  define LPC_PORT_DEFINED_STDINT_GUARD 1
#endif
#if defined(_MSC_VER) && !defined(_STDINT_H_)
#  define _STDINT_H_
#  define LPC_PORT_DEFINED_STDINT_GUARD_MSVC 1
#endif
#include "libc.h"

/* allocator.h supplies the only allocation backend used by this module.  In
 * freestanding kernel syntax checks, lpc_port.h already supplied compatible
 * BOOLEAN/ULONG declarations, so suppress allocator.h's duplicate fallbacks. */
#if defined(_KERNEL_MODE) && !LPC_PORT_HAS_PLATFORM_HEADERS && \
    !defined(ALLOCATOR_HAVE_WDK)
#  define ALLOCATOR_HAVE_WDK 1
#  define LPC_PORT_DEFINED_ALLOCATOR_HAVE_WDK 1
#endif
#include "allocator.h"
#ifdef LPC_PORT_DEFINED_ALLOCATOR_HAVE_WDK
#  undef ALLOCATOR_HAVE_WDK
#  undef LPC_PORT_DEFINED_ALLOCATOR_HAVE_WDK
#endif

#if !LPC_PORT_HAS_PLATFORM_HEADERS && defined(_MSC_VER)
#  include <intrin.h>
#endif

#ifndef STATUS_SUCCESS
#  define STATUS_SUCCESS ((NTSTATUS)(LONG)0x00000000UL)
#endif
#ifndef STATUS_UNSUCCESSFUL
#  define STATUS_UNSUCCESSFUL ((NTSTATUS)(LONG)0xC0000001UL)
#endif
#ifndef STATUS_INVALID_PARAMETER
#  define STATUS_INVALID_PARAMETER ((NTSTATUS)(LONG)0xC000000DUL)
#endif
#ifndef STATUS_INFO_LENGTH_MISMATCH
#  define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)(LONG)0xC0000004UL)
#endif
#ifndef STATUS_DATA_ERROR
#  define STATUS_DATA_ERROR ((NTSTATUS)(LONG)0xC000003EUL)
#endif
#ifndef STATUS_INSUFFICIENT_RESOURCES
#  define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)(LONG)0xC000009AUL)
#endif
#ifndef STATUS_NOT_SUPPORTED
#  define STATUS_NOT_SUPPORTED ((NTSTATUS)(LONG)0xC00000BBUL)
#endif
#ifndef STATUS_DEVICE_BUSY
#  define STATUS_DEVICE_BUSY ((NTSTATUS)(LONG)0xC00000E8UL)
#endif
#ifndef STATUS_TIMEOUT
#  define STATUS_TIMEOUT ((NTSTATUS)(LONG)0x00000102UL)
#endif
#ifndef STATUS_PORT_DISCONNECTED
#  define STATUS_PORT_DISCONNECTED ((NTSTATUS)(LONG)0xC0000037UL)
#endif
#ifndef NT_SUCCESS
#  define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#endif

#ifndef ANYSIZE_ARRAY
#  define ANYSIZE_ARRAY 1
#endif


/* Windows headers expose some NTSTATUS constants as unsigned literals.  Cast
 * at API boundaries so strict sign-conversion builds remain warning-free. */
#define LPC_STATUS(value) ((NTSTATUS)(value))

/* A byte buffer is used for variable-sized messages, while the union keeps
 * its address suitably aligned for LPC_PORT_MESSAGE on 64-bit builds. */
typedef union _LPC_MESSAGE_BUFFER {
    LPC_HEADER alignment;
    UCHAR bytes[LPC_PORT_BUFFER_SIZE];
} LPC_MESSAGE_BUFFER;

/* Context shutdown can race event-loop and send threads.  Keep the public
 * context layout simple, but publish its state with acquire/release atomics
 * so a close is claimed by exactly one caller. */
static LONG lpc_state_load(const volatile LONG *state)
{
#if defined(_MSC_VER)
    return InterlockedCompareExchange((volatile LONG *)state, 0, 0);
#elif defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(state, __ATOMIC_ACQUIRE);
#else
    return *state;
#endif
}

static void lpc_state_store(volatile LONG *state, LONG value)
{
#if defined(_MSC_VER)
    (void)InterlockedExchange(state, value);
#elif defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(state, value, __ATOMIC_RELEASE);
#else
    *state = value;
#endif
}

static LONG lpc_state_compare_exchange(volatile LONG *state,
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

static int lpc_state_claim_close(volatile LONG *state)
{
    return state != (volatile LONG *)0 &&
           lpc_state_compare_exchange(state, 1, 0) == 1;
}

static int lpc_lock_init(LPC_PORT_LOCK *lock)
{
    if (!lock) {
        return 0;
    }
#if defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    ExInitializePushLock(&lock->native);
#elif !defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    InitializeCriticalSection(&lock->native);
#else
    lock->native = 0;
#endif
    lock->initialized = 1;
    return 1;
}

static void lpc_lock_destroy(LPC_PORT_LOCK *lock)
{
    if (!lock || !lock->initialized) {
        return;
    }
#if !defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    DeleteCriticalSection(&lock->native);
#elif !defined(_KERNEL_MODE) && !LPC_PORT_HAS_PLATFORM_HEADERS
    lock->native = 0;
#endif
    lock->initialized = 0;
}

static void lpc_lock_acquire(LPC_PORT_LOCK *lock)
{
    if (!lock || !lock->initialized) {
        return;
    }
#if defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    ExAcquirePushLockExclusive(&lock->native);
#elif !defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
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

static void lpc_lock_release(LPC_PORT_LOCK *lock)
{
    if (!lock || !lock->initialized) {
        return;
    }
#if defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    ExReleasePushLockExclusive(&lock->native);
#elif !defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    LeaveCriticalSection(&lock->native);
#elif defined(_MSC_VER)
    _InterlockedExchange((volatile long *)&lock->native, 0);
#elif defined(__GNUC__) || defined(__clang__)
    __sync_lock_release(&lock->native);
#else
    lock->native = 0U;
#endif
}

#if !LPC_PORT_HAS_PLATFORM_HEADERS
/* The fallback lifetime object doubles its native word as a tiny spin lock.
 * This keeps active/closing updates race-free when a test harness builds
 * without the Windows synchronization primitives. */
static void lpc_lifetime_fallback_lock(LPC_PORT_LIFETIME *life)
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

static void lpc_lifetime_fallback_unlock(LPC_PORT_LIFETIME *life)
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

#if defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
/* Serialize the short closing/admission transition before touching rundown. */
static void lpc_lifetime_admission_lock(LPC_PORT_LIFETIME *life)
{
    if (!life) {
        return;
    }
    while (InterlockedCompareExchange(&life->admission, 1, 0) != 0) {
        KeYieldProcessor();
    }
}

static void lpc_lifetime_admission_unlock(LPC_PORT_LIFETIME *life)
{
    if (life) {
        (void)InterlockedExchange(&life->admission, 0);
    }
}
#endif

/* Lifetime helpers prevent close/reset from racing a Native call. */
static int lpc_lifetime_init(LPC_PORT_LIFETIME *life)
{
    if (!life) {
        return 0;
    }
#if defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    ExInitializeRundownProtection(&life->rundown);
    life->admission = 0;
    life->closing = 0;
    life->initialized = 1;
#elif !defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
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
    return 1;
}

static int lpc_lifetime_acquire(LPC_PORT_LIFETIME *life)
{
    if (!life || !life->initialized) {
        return 0;
    }
#if defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    {
        int acquired = 0;
        lpc_lifetime_admission_lock(life);
        if (life->initialized && life->closing == 0 &&
            ExAcquireRundownProtection(&life->rundown)) {
            acquired = 1;
        }
        lpc_lifetime_admission_unlock(life);
        return acquired;
    }
#elif !defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    {
        int acquired = 0;
        EnterCriticalSection(&life->native);
        if (life->initialized && !life->closing) {
            ++life->active;
            acquired = 1;
        }
        LeaveCriticalSection(&life->native);
        return acquired;
    }
#else
    {
        int acquired = 0;
        lpc_lifetime_fallback_lock(life);
        if (life->initialized && !life->closing) {
            ++life->active;
            acquired = 1;
        }
        lpc_lifetime_fallback_unlock(life);
        return acquired;
    }
#endif
}

static void lpc_lifetime_release(LPC_PORT_LIFETIME *life)
{
    if (!life) {
        return;
    }
#if defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    ExReleaseRundownProtection(&life->rundown);
#elif !defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    EnterCriticalSection(&life->native);
    if (life->active) {
        --life->active;
    }
    if (life->closing && life->active == 0) {
        WakeAllConditionVariable(&life->idle);
    }
    LeaveCriticalSection(&life->native);
#else
    lpc_lifetime_fallback_lock(life);
    if (life->active) {
        --life->active;
    }
    lpc_lifetime_fallback_unlock(life);
#endif
}

/* Mark shutdown before closing a native handle.  This closes the admission
 * window between a caller's state check and its lifetime pin. */
static void lpc_lifetime_mark_closing(LPC_PORT_LIFETIME *life)
{
    if (!life || !life->initialized) {
        return;
    }
#if defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    lpc_lifetime_admission_lock(life);
    life->closing = 1;
    lpc_lifetime_admission_unlock(life);
#elif !defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    EnterCriticalSection(&life->native);
    life->closing = 1;
    LeaveCriticalSection(&life->native);
#else
    lpc_lifetime_fallback_lock(life);
    life->closing = 1;
    lpc_lifetime_fallback_unlock(life);
#endif
}

/* Drain pins after the native handle has been disconnected/closed. */
static void lpc_lifetime_wait(LPC_PORT_LIFETIME *life)
{
    if (!life || !life->initialized) {
        return;
    }
#if defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    ExWaitForRundownProtectionRelease(&life->rundown);
#elif !defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    EnterCriticalSection(&life->native);
    while (life->active != 0) {
        SleepConditionVariableCS(&life->idle, &life->native, INFINITE);
    }
    LeaveCriticalSection(&life->native);
#else
    for (;;) {
        uint32_t active = 0;
        lpc_lifetime_fallback_lock(life);
        active = life->active;
        lpc_lifetime_fallback_unlock(life);
        if (active == 0U) {
            break;
        }
#if defined(_MSC_VER)
        YieldProcessor();
#endif
    }
#endif
}

static void lpc_lifetime_destroy(LPC_PORT_LIFETIME *life)
{
    if (!life) {
        return;
    }
#if !defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
    if (life->initialized) {
        DeleteCriticalSection(&life->native);
    }
#endif
    life->initialized = 0;
}

static int lpc_client_api_valid(const LPC_CLIENT_APIS *api) {
    return api && api->pfnRtlInitAnsiString && api->pfnNtCreateSection &&
           api->pfnNtConnectPort && api->pfnNtClose &&
           api->pfnRtlAnsiStringToUnicodeString && api->pfnRtlFreeUnicodeString &&
           api->pfnNtRequestPort && api->pfnNtRequestWaitReplyPort;
}

static int lpc_server_api_valid(const LPC_SERVER_APIS *api) {
    return api && api->pfnRtlInitAnsiString && api->pfnNtCreatePort &&
           api->pfnNtClose && api->pfnRtlAnsiStringToUnicodeString &&
           api->pfnRtlFreeUnicodeString &&
           (api->pfnNtReplyWaitReceivePort || api->pfnNtReplyWaitReceivePortEx) &&
           api->pfnNtAcceptConnectPort && api->pfnNtCompleteConnectPort &&
           api->pfnNtReplyPort;
}

static int lpc_port_name_valid(const LPC_PORT_NAME *name) {
    size_t length = 0;

    if (!name) {
        return 0;
    }
    length = libc_strnlen_s(name->name, sizeof(name->name));
    return length > 0 && length < sizeof(name->name);
}

static void lpc_init_port_message(PLPC_PORT_MESSAGE message, size_t totalLength, USHORT type) {
    if (!message || totalLength < sizeof(LPC_PORT_MESSAGE) || totalLength > 0xffffU) {
        return;
    }

    libc_memset(message, 0, sizeof(*message));
    message->u1.s1.TotalLength = (USHORT)totalLength;
    message->u1.s1.DataLength = (USHORT)(totalLength - sizeof(LPC_PORT_MESSAGE));
    message->u2.s2.Type = type;
}

static int lpc_port_message_valid(const PLPC_PORT_MESSAGE message, size_t bufferSize,
                                  size_t minimumLength) {
    size_t totalLength = 0;

    if (!message) {
        return 0;
    }
    totalLength = message->u1.s1.TotalLength;
    return totalLength >= sizeof(LPC_PORT_MESSAGE) && totalLength >= minimumLength &&
           totalLength <= bufferSize &&
           message->u1.s1.DataLength == totalLength - sizeof(LPC_PORT_MESSAGE);
}

/* Build a protocol-valid empty inline reply.  LPC has no status field in the
 * wire payload, but replying is still important: a malformed synchronous
 * request must not leave its caller blocked forever in NtRequestWaitReplyPort. */
static int lpc_prepare_empty_reply(PLPC_HEADER header, size_t bufferSize)
{
    LPC_PORT_MESSAGE nativeHeader = {0};
    PLPC_MESSAGE message = (PLPC_MESSAGE)0;
    size_t totalLength = LPC_HEADER_DATA_OFFSET + sizeof(ULONG);
    uint8_t useSharedMemory = 0;

    if (!header || bufferSize < totalLength) {
        return 0;
    }
    /* Keep the MessageId/ClientId fields generated by the kernel.  Rebuilding
     * the whole PORT_MESSAGE with lpc_init_port_message would erase them and
     * NtReplyPort could no longer match this reply to its waiting sender. */
    nativeHeader = header->header;
    useSharedMemory = header->u1.s1.ulUseSharedMemory ? 1U : 0U;
    message = (PLPC_MESSAGE)(void *)header->data;
    libc_memset(message, 0, sizeof(ULONG));
    /* Preserve the transport mode so a shared-memory caller does not parse an
     * inline error envelope as a reply from a different storage area. */
    header->u1.s1.ulUseSharedMemory = useSharedMemory != 0U;
    header->u1.s1.ulUseAsyncMethod = 0;
    header->u1.s1.ulReserved = 0;
    header->header = nativeHeader;
    header->header.u1.s1.TotalLength = (USHORT)totalLength;
    header->header.u1.s1.DataLength =
        (USHORT)(totalLength - sizeof(LPC_PORT_MESSAGE));
    header->header.u2.s2.Type = (USHORT)LPC_TYPE_REPLY;
    header->header.u2.s2.DataInfoOffset = 0;
    return lpc_port_message_valid(&header->header, bufferSize, totalLength);
}

static void lpc_clear_shared_memory(const PLPC_SERVER_CLIENT_INFO client)
{
    PLPC_SHARED_MEMORY shared = (PLPC_SHARED_MEMORY)0;

    if (!client || !client->client_view.ViewBase ||
        client->client_view.ViewSize < sizeof(ULONG)) {
        return;
    }
    shared = (PLPC_SHARED_MEMORY)client->client_view.ViewBase;
    shared->size = 0;
}

/* Once an LPC connection request has been dequeued, simply returning an
 * error leaves the client waiting forever in NtConnectPort.  Use the native
 * reject path for every malformed request that still has a valid PORT_MESSAGE
 * type field; the returned handle is only a transient reject handle. */
static NTSTATUS lpc_reject_connection(const LPC_SERVER_CONTEXT *context,
                                       PLPC_PORT_MESSAGE request)
{
    LPC_PORT_MESSAGE normalized = {0};
    HANDLE rejectedHandle = NULL;
    NTSTATUS status = STATUS_DATA_ERROR;

    if (!context || !request || !context->hLPCPortServerHandle ||
        !context->api.pfnNtAcceptConnectPort) {
        return STATUS_DATA_ERROR;
    }
    /* NtAcceptConnectPort still expects a structurally valid native header
     * even when AcceptConnection is FALSE.  Preserve the kernel routing
     * fields but normalize the user-controlled length/type fields so a
     * truncated request can be rejected instead of leaving NtConnectPort
     * pending forever. */
    normalized = *request;
    normalized.u1.s1.TotalLength =
        (USHORT)(sizeof(LPC_PORT_MESSAGE) + sizeof(uint32_t));
    normalized.u1.s1.DataLength = (USHORT)sizeof(uint32_t);
    normalized.u2.s2.Type = (USHORT)LPC_TYPE_CONNECTION_REQUEST;
    normalized.u2.s2.DataInfoOffset = 0;
    status = context->api.pfnNtAcceptConnectPort(
        &rejectedHandle, NULL, &normalized, (BOOLEAN)0, NULL, NULL);
    if (rejectedHandle && context->api.pfnNtClose) {
        (void)context->api.pfnNtClose(rejectedHandle);
    }
    return status;
}

static int lpc_inline_message(const PLPC_HEADER header, size_t bufferSize,
                              PLPC_MESSAGE *message, size_t *available) {
    size_t payloadLength = 0;
    PLPC_MESSAGE localMessage = NULL;

    if (!header ||
        !lpc_port_message_valid(&header->header, bufferSize, LPC_HEADER_DATA_OFFSET + sizeof(ULONG))) {
        return 0;
    }

    payloadLength = header->header.u1.s1.TotalLength - LPC_HEADER_DATA_OFFSET;
    if (payloadLength < sizeof(ULONG)) {
        return 0;
    }
    localMessage = (PLPC_MESSAGE)(void *)header->data;
    if ((size_t)localMessage->size > payloadLength - sizeof(ULONG) ||
        localMessage->size > LPC_MESSAGE_MAX_PACK_SIZE) {
        return 0;
    }
    if ((size_t)localMessage->size != payloadLength - sizeof(ULONG)) {
        return 0;
    }
    if (message) {
        *message = localMessage;
    }
    if (available) {
        *available = payloadLength - sizeof(ULONG);
    }
    return 1;
}

static int lpc_shared_message(const PLPC_SHARED_MEMORY shared, SIZE_T viewSize,
                              uint32_t *size, uint8_t **data) {
    if (!shared || viewSize < sizeof(ULONG) || shared->size > viewSize - sizeof(ULONG)) {
        return 0;
    }
    if (size) {
        *size = shared->size;
    }
    if (data) {
        *data = shared->msg;
    }
    return 1;
}

/* NtReplyWaitReceivePort returns the PortContext supplied to
 * NtAcceptConnectPort, not the communication-port handle.  Match both forms
 * so resolver test doubles that return a handle remain usable. */
static PLPC_SERVER_CLIENT_INFO lpc_acquire_client_by_context(
    PLPC_SERVER_CONTEXT context, PVOID portContext)
{
    PLPC_SERVER_CLIENT_INFO current = NULL;

    if (!context || !portContext) {
        return NULL;
    }
    lpc_lock_acquire(&context->lockWord);
    for (current = (PLPC_SERVER_CLIENT_INFO)List_Head(&context->clientList);
         current;
         current = (PLPC_SERVER_CLIENT_INFO)List_Next(&current->listEntry)) {
        if ((PVOID)current == portContext ||
            current->hLPCPortClientHandle == (HANDLE)portContext) {
            if (!lpc_lifetime_acquire(&current->lifetime)) {
                current = NULL;
            }
            lpc_lock_release(&context->lockWord);
            return current;
        }
    }
    lpc_lock_release(&context->lockWord);
    return NULL;
}

static PLPC_SERVER_CLIENT_INFO lpc_detach_client(PLPC_SERVER_CONTEXT context, HANDLE clientHandle) {
    PLPC_SERVER_CLIENT_INFO current = NULL;

    if (!context || !clientHandle) {
        return NULL;
    }
    lpc_lock_acquire(&context->lockWord);
    for (current = (PLPC_SERVER_CLIENT_INFO)List_Head(&context->clientList);
         current;
         current = (PLPC_SERVER_CLIENT_INFO)List_Next(&current->listEntry)) {
        if (current->hLPCPortClientHandle == clientHandle) {
            if (List_Remove(&context->clientList, &current->listEntry)) {
                if (context->clientCount) {
                    --context->clientCount;
                }
                lpc_lock_release(&context->lockWord);
                /* The caller must wake the Native receive before beginning
                 * rundown.  Waiting here would deadlock if another worker
                 * is blocked in NtReplyWaitReceivePort on this endpoint. */
                return current;
            }
            break;
        }
    }
    lpc_lock_release(&context->lockWord);
    return NULL;
}

static int lpc_add_client(PLPC_SERVER_CONTEXT context, PLPC_SERVER_CLIENT_INFO info) {
    PLPC_SERVER_CLIENT_INFO current = NULL;

    if (!context || !info) {
        return 0;
    }
    if (!info->lifetime.initialized && !lpc_lifetime_init(&info->lifetime)) {
        return 0;
    }
    if (!info->receiveLock.initialized && !lpc_lock_init(&info->receiveLock)) {
        lpc_lifetime_destroy(&info->lifetime);
        return 0;
    }
    lpc_lock_acquire(&context->lockWord);
    if (context->maxClients && context->clientCount >= context->maxClients) {
        lpc_lock_release(&context->lockWord);
        lpc_lock_destroy(&info->receiveLock);
        lpc_lifetime_destroy(&info->lifetime);
        return 0;
    }
    for (current = (PLPC_SERVER_CLIENT_INFO)List_Head(&context->clientList);
         current;
         current = (PLPC_SERVER_CLIENT_INFO)List_Next(&current->listEntry)) {
        if (current->hLPCPortClientHandle == info->hLPCPortClientHandle) {
            lpc_lock_release(&context->lockWord);
            lpc_lock_destroy(&info->receiveLock);
            lpc_lifetime_destroy(&info->lifetime);
            return 0;
        }
    }
    if (List_Count(&context->clientList) >= (size_t)(uint32_t)-1 ||
        !List_Insert_After(&context->clientList, List_Tail(&context->clientList),
                           &info->listEntry)) {
        lpc_lock_release(&context->lockWord);
        lpc_lock_destroy(&info->receiveLock);
        lpc_lifetime_destroy(&info->lifetime);
        return 0;
    }
    ++context->clientCount;
    lpc_lock_release(&context->lockWord);
    return 1;
}

static void lpc_snapshot_events(PLPC_SERVER_CONTEXT context, LPC_SERVER_EVT_CONTEXT *events) {
    if (!context || !events) {
        return;
    }
    lpc_lock_acquire(&context->lockWord);
    *events = context->ServerEvtCallback;
    lpc_lock_release(&context->lockWord);
}

static void lpc_close_client_info(PLPC_SERVER_CLIENT_INFO info) {
    if (!info) {
        return;
    }
    lpc_lock_destroy(&info->receiveLock);
    lpc_lifetime_destroy(&info->lifetime);
#if defined(_KERNEL_MODE)
    Allocator_Free(info, (ULONG)LPC_POOL_TAG);
#else
    Allocator_Free(info);
#endif
}

/* Detach and dispose one communication endpoint exactly once.  Removing the
 * record before invoking the callback makes a concurrent PORT_CLOSED event or
 * explicit disconnect observe the missing entry and skip a duplicate close
 * notification.  The caller must hold a server lifetime pin while using this
 * helper; the per-client rundown is drained before its storage is released. */
static NTSTATUS lpc_drop_client(PLPC_SERVER_CONTEXT context,
                                HANDLE clientHandle,
                                uint8_t notify)
{
    PLPC_SERVER_CLIENT_INFO client = NULL;
    LPC_SERVER_EVT_CONTEXT events = {0};

    if (!context || !clientHandle) {
        return LPC_STATUS(STATUS_INVALID_PARAMETER);
    }
    client = lpc_detach_client(context, clientHandle);
    if (!client) {
        return LPC_STATUS(STATUS_INVALID_PARAMETER);
    }
    lpc_snapshot_events(context, &events);
    lpc_lifetime_mark_closing(&client->lifetime);
    if (context->api.pfnNtClose && client->hLPCPortClientHandle) {
        (void)context->api.pfnNtClose(client->hLPCPortClientHandle);
    }
    /* Closing wakes a receive blocked in the Native API.  Take the callback
     * lock only after the wake-up: a worker pins the client lifetime before
     * waiting for receiveLock and may otherwise be asleep in the Native call. */
    lpc_lock_acquire(&client->receiveLock);
    /* The callback receives the endpoint value for identification; LPC has no
     * disconnect primitive, so the native handle may already be closed here. */
    if (notify && events.onClose) {
        events.onClose(clientHandle);
    }
    lpc_lock_release(&client->receiveLock);
    lpc_lifetime_wait(&client->lifetime);
    lpc_close_client_info(client);
    return STATUS_SUCCESS;
}

static void *lpc_alloc_memory(size_t size)
{
    if (!size) {
        return NULL;
    }
#if defined(_KERNEL_MODE)
    return Allocator_Malloc((BOOLEAN)1, size, (ULONG)LPC_POOL_TAG);
#else
    return Allocator_Malloc(size);
#endif
}

static void lpc_free_memory(void *memory)
{
    if (!memory) {
        return;
    }
#if defined(_KERNEL_MODE)
    Allocator_Free(memory, (ULONG)LPC_POOL_TAG);
#else
    Allocator_Free(memory);
#endif
}

static void lpc_reset_client_context(PLPC_CLIENT_CONTEXT context) {
    if (context) {
        context->hLPCPortHandle = NULL;
        context->hSectionHandle = NULL;
        libc_memset(&context->ustrLPCName, 0, sizeof(context->ustrLPCName));
        libc_memset(&context->client_view, 0, sizeof(context->client_view));
        libc_memset(&context->api, 0, sizeof(context->api));
        lpc_state_store(&context->initialized, 0);
    }
}

static void lpc_reset_server_context(PLPC_SERVER_CONTEXT context) {
    if (context) {
        context->hLPCPortServerHandle = NULL;
        libc_memset(&context->ustrLPCName, 0, sizeof(context->ustrLPCName));
        libc_memset(&context->ServerEvtCallback, 0, sizeof(context->ServerEvtCallback));
        libc_memset(&context->clientList, 0, sizeof(context->clientList));
        context->clientCount = 0;
        context->maxClients = 0;
        libc_memset(&context->api, 0, sizeof(context->api));
        lpc_state_store(&context->initialized, 0);
    }
}

NTSTATUS LpcPort_ServerCreate(PLPC_SERVER_CONFIG config, PLPC_SERVER_CONTEXT context) {
    ANSI_STRING ansiName = {0};
    UNICODE_STRING unicodeName = {0};
    OBJECT_ATTRIBUTES objectAttributes = {0};
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    if (!config || !context || !lpc_port_name_valid(&config->LpcName)) {
        return LPC_STATUS(STATUS_INVALID_PARAMETER);
    }
    if (lpc_state_load(&context->initialized) != 0 || context->hLPCPortServerHandle ||
        context->ustrLPCName.Buffer || context->lockWord.initialized ||
        context->lifetime.initialized) {
        return STATUS_DEVICE_BUSY;
    }
    lpc_reset_server_context(context);
    context->api = config->api;
    if (!lpc_server_api_valid(&context->api)) {
        lpc_reset_server_context(context);
        return LPC_STATUS(STATUS_INVALID_PARAMETER);
    }
    if (!lpc_lifetime_init(&context->lifetime)) {
        lpc_reset_server_context(context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (
        !List_Init(&context->clientList) || !lpc_lock_init(&context->lockWord)) {
        lpc_lifetime_destroy(&context->lifetime);
        lpc_reset_server_context(context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    libc_memset(&ansiName, 0, sizeof(ansiName));
    libc_memset(&unicodeName, 0, sizeof(unicodeName));
    libc_memset(&objectAttributes, 0, sizeof(objectAttributes));

    context->api.pfnRtlInitAnsiString(&ansiName, config->LpcName.name);
    status = context->api.pfnRtlAnsiStringToUnicodeString(&unicodeName, &ansiName, (BOOLEAN)1);
    if (!NT_SUCCESS(status)) {
        lpc_lock_destroy(&context->lockWord);
        lpc_lifetime_destroy(&context->lifetime);
        lpc_reset_server_context(context);
        return status;
    }
    context->ustrLPCName = unicodeName;

#if LPC_PORT_HAS_PLATFORM_HEADERS
    InitializeObjectAttributes(&objectAttributes, &context->ustrLPCName,
                               OBJ_CASE_INSENSITIVE, NULL, NULL);
#else
    objectAttributes.Length = (ULONG)sizeof(objectAttributes);
    objectAttributes.Attributes = OBJ_CASE_INSENSITIVE;
    objectAttributes.ObjectName = &context->ustrLPCName;
#endif

    objectAttributes.SecurityDescriptor = config->securityDescriptor;

    status = context->api.pfnNtCreatePort(&context->hLPCPortServerHandle, &objectAttributes,
                                          (ULONG)sizeof(uint32_t),
                                          (ULONG)LPC_PORT_NATIVE_MAX_MESSAGE_LENGTH, 0);
    if (!NT_SUCCESS(status) || !context->hLPCPortServerHandle) {
        if (NT_SUCCESS(status)) {
            status = STATUS_DATA_ERROR;
        }
        if (context->hLPCPortServerHandle && context->api.pfnNtClose) {
            (void)context->api.pfnNtClose(context->hLPCPortServerHandle);
        }
        context->api.pfnRtlFreeUnicodeString(&context->ustrLPCName);
        lpc_lock_destroy(&context->lockWord);
        lpc_lifetime_destroy(&context->lifetime);
        lpc_reset_server_context(context);
        return status;
    }
    context->maxClients = config->maxClients;
    lpc_state_store(&context->initialized, 1);
    return STATUS_SUCCESS;
}

NTSTATUS LpcPort_Connect(PLPC_CLIENT_CONFIG config, PLPC_CLIENT_CONTEXT context) {
    ANSI_STRING ansiName = {0};
    UNICODE_STRING unicodeName = {0};
    LARGE_INTEGER sectionSize = {0};
    SECURITY_QUALITY_OF_SERVICE securityQos = {0};
    ULONG controlId = 0;
    ULONG controlIdLength = 0;
    ULONG maxMessageLength = 0;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    if (!config || !context || !lpc_port_name_valid(&config->LpcName)) {
        return LPC_STATUS(STATUS_INVALID_PARAMETER);
    }
    if (lpc_state_load(&context->initialized) != 0 || context->hLPCPortHandle ||
        context->hSectionHandle || context->ustrLPCName.Buffer ||
        context->sendLock.initialized || context->lifetime.initialized) {
        return STATUS_DEVICE_BUSY;
    }
    lpc_reset_client_context(context);
    context->api = config->api;
    if (!lpc_client_api_valid(&context->api)) {
        lpc_reset_client_context(context);
        return LPC_STATUS(STATUS_INVALID_PARAMETER);
    }
    if (!lpc_lifetime_init(&context->lifetime)) {
        lpc_reset_client_context(context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!lpc_lock_init(&context->sendLock)) {
        lpc_lifetime_destroy(&context->lifetime);
        lpc_reset_client_context(context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    libc_memset(&ansiName, 0, sizeof(ansiName));
    libc_memset(&unicodeName, 0, sizeof(unicodeName));
    libc_memset(&sectionSize, 0, sizeof(sectionSize));
    libc_memset(&securityQos, 0, sizeof(securityQos));

    context->api.pfnRtlInitAnsiString(&ansiName, config->LpcName.name);
    status = context->api.pfnRtlAnsiStringToUnicodeString(&unicodeName, &ansiName, (BOOLEAN)1);
    if (!NT_SUCCESS(status)) {
        lpc_lock_destroy(&context->sendLock);
        lpc_lifetime_destroy(&context->lifetime);
        lpc_reset_client_context(context);
        return status;
    }
    context->ustrLPCName = unicodeName;

    sectionSize.QuadPart = (int64_t)LPC_SECTION_MAX_SPACE_SIZE;
    status = context->api.pfnNtCreateSection(&context->hSectionHandle,
                                             (ACCESS_MASK)(SECTION_MAP_READ | SECTION_MAP_WRITE), NULL,
                                             &sectionSize, PAGE_READWRITE, SEC_COMMIT, NULL);
    if (!NT_SUCCESS(status) || !context->hSectionHandle) {
        if (NT_SUCCESS(status)) {
            status = STATUS_DATA_ERROR;
        }
        goto failure;
    }

    securityQos.Length = (ULONG)sizeof(securityQos);
    securityQos.ImpersonationLevel = SecurityImpersonation;
    securityQos.EffectiveOnly = (BOOLEAN)0;
    securityQos.ContextTrackingMode = (BOOLEAN)SECURITY_DYNAMIC_TRACKING;
    context->client_view.Length = (ULONG)sizeof(context->client_view);
    context->client_view.SectionHandle = context->hSectionHandle;
    context->client_view.SectionOffset = 0;
    context->client_view.ViewSize = (SIZE_T)LPC_SECTION_MAX_SPACE_SIZE;
    context->client_view.ViewBase = NULL;
    context->client_view.ViewRemoteBase = NULL;

    controlId = config->HelloId;
    controlIdLength = (ULONG)sizeof(controlId);
    status = context->api.pfnNtConnectPort(&context->hLPCPortHandle, &context->ustrLPCName,
                                           &securityQos, &context->client_view, NULL,
                                           &maxMessageLength, &controlId, &controlIdLength);
    if (!NT_SUCCESS(status) || !context->hLPCPortHandle) {
        if (NT_SUCCESS(status)) {
            status = STATUS_DATA_ERROR;
        }
        goto failure;
    }
    if (controlIdLength != (ULONG)sizeof(controlId) ||
        (maxMessageLength != 0 &&
         maxMessageLength < (ULONG)LPC_HEADER_DATA_OFFSET)) {
        status = STATUS_DATA_ERROR;
        goto failure;
    }
    lpc_state_store(&context->initialized, 1);
    if (config->lpRespID) {
        *config->lpRespID = controlId;
    }
    return STATUS_SUCCESS;

failure:
    if (context->hLPCPortHandle) {
        context->api.pfnNtClose(context->hLPCPortHandle);
    }
    if (context->hSectionHandle) {
        context->api.pfnNtClose(context->hSectionHandle);
    }
    if (context->ustrLPCName.Buffer) {
        context->api.pfnRtlFreeUnicodeString(&context->ustrLPCName);
    }
    lpc_lock_destroy(&context->sendLock);
    lpc_lifetime_destroy(&context->lifetime);
    lpc_reset_client_context(context);
    return status;
}

void LpcPort_DisConnect(PLPC_CLIENT_CONTEXT context) {
    if (!context || !lpc_state_claim_close(&context->initialized)) {
        return;
    }
    /* lpc_state_claim_close stopped new sends; wait for the last sender. */
    /* NtRequestWaitReplyPort has no timeout/cancel argument.  Closing the
     * port first is therefore required to wake a sender that is currently in
     * the kernel; only after that can rundown safely wait for it to return. */
    lpc_lifetime_mark_closing(&context->lifetime);
    if (context->api.pfnNtClose && context->hLPCPortHandle) {
        context->api.pfnNtClose(context->hLPCPortHandle);
    }
    lpc_lifetime_wait(&context->lifetime);
    if (context->api.pfnNtClose && context->hSectionHandle) {
        context->api.pfnNtClose(context->hSectionHandle);
    }
    if (context->api.pfnRtlFreeUnicodeString && context->ustrLPCName.Buffer) {
        context->api.pfnRtlFreeUnicodeString(&context->ustrLPCName);
    }
    lpc_lock_destroy(&context->sendLock);
    lpc_lifetime_destroy(&context->lifetime);
    lpc_reset_client_context(context);
}

void LpcPort_Disconnect(PLPC_CLIENT_CONTEXT context)
{
    LpcPort_DisConnect(context);
}

void LpcPort_ServerClose(PLPC_SERVER_CONTEXT context) {
    PLPC_SERVER_CLIENT_INFO client = NULL;
    LPC_SERVER_EVT_CONTEXT closeEvents = {0};

    if (!context) {
        return;
    }
    if (!lpc_state_claim_close(&context->initialized)) {
        return;
    }
    /* lpc_state_claim_close makes a concurrent close a harmless no-op while
     * this thread drains active users. */
    /* NtReplyWaitReceivePort has no cancellation parameter.  Close the
     * connection endpoint first so blocked event processors wake with a port
     * error; rundown then drains those processors before client records and
     * callbacks are destroyed. */
    lpc_lifetime_mark_closing(&context->lifetime);
    if (context->api.pfnNtClose && context->hLPCPortServerHandle) {
        context->api.pfnNtClose(context->hLPCPortServerHandle);
    }
    lpc_lifetime_wait(&context->lifetime);
    lpc_snapshot_events(context, &closeEvents);
    context->hLPCPortServerHandle = NULL;
    if (context->api.pfnRtlFreeUnicodeString && context->ustrLPCName.Buffer) {
        context->api.pfnRtlFreeUnicodeString(&context->ustrLPCName);
    }
    context->ustrLPCName.Buffer = NULL;
    context->ustrLPCName.Length = 0;
    context->ustrLPCName.MaximumLength = 0;

    for (;;) {
        lpc_lock_acquire(&context->lockWord);
        client = (PLPC_SERVER_CLIENT_INFO)List_Head(&context->clientList);
        if (!client || !List_Remove(&context->clientList, &client->listEntry)) {
            /* A corrupted list must not turn shutdown into an infinite loop. */
            if (client) {
                List_Clear(&context->clientList);
            }
            context->clientCount = 0;
            lpc_lock_release(&context->lockWord);
            break;
        }
        if (context->clientCount) {
            --context->clientCount;
        }
        lpc_lock_release(&context->lockWord);

        /* LPC exposes no disconnect primitive, so closing the communication
         * handle is the only way to wake a worker blocked in the Native
         * receive.  The server lifetime was drained above, so no worker can
         * still be using this record; keep the per-client lock ordering the
         * same as the explicit-disconnect path for consistency. */
        lpc_lifetime_mark_closing(&client->lifetime);
        if (client->hLPCPortClientHandle && context->api.pfnNtClose) {
            (void)context->api.pfnNtClose(client->hLPCPortClientHandle);
        }
        lpc_lock_acquire(&client->receiveLock);
        lpc_lifetime_wait(&client->lifetime);
        if (closeEvents.onClose) {
            closeEvents.onClose(client->hLPCPortClientHandle);
        }
        lpc_lock_release(&client->receiveLock);
        lpc_close_client_info(client);
    }
    lpc_lock_acquire(&context->lockWord);
    libc_memset(&context->ServerEvtCallback, 0, sizeof(context->ServerEvtCallback));
    lpc_lock_release(&context->lockWord);
    lpc_lock_destroy(&context->lockWord);
    lpc_lifetime_destroy(&context->lifetime);
    lpc_reset_server_context(context);
}

NTSTATUS LpcPort_ServerDisconnectClient(PLPC_SERVER_CONTEXT context,
                                         HANDLE clientHandle)
{
    NTSTATUS status = LPC_STATUS(STATUS_INVALID_PARAMETER);

    if (!context || lpc_state_load(&context->initialized) == 0 || !clientHandle ||
        !lpc_lifetime_acquire(&context->lifetime)) {
        return LPC_STATUS(STATUS_INVALID_PARAMETER);
    }
    status = lpc_drop_client(context, clientHandle, 1);
    lpc_lifetime_release(&context->lifetime);
    return status;
}

NTSTATUS LpcPort_SendMessage(PLPC_CLIENT_CONTEXT context, const void *message,
                                   ULONG messageLength, ULONG controlId, uint8_t useAsyncMode,
                                   uint8_t useSharedMemory,
                                   typedef_LpcSyncMsgReplyCallback replyCallback, void *callbackContext) {
    LPC_MESSAGE_BUFFER requestStorage = {0};
    LPC_MESSAGE_BUFFER replyStorage = {0};
    UCHAR *requestBuffer = NULL;
    UCHAR *replyBuffer = NULL;
    LPC_HEADER sharedHeader = {0};
    PLPC_HEADER request = NULL;
    PLPC_HEADER reply = NULL;
    PLPC_MESSAGE inlineMessage = NULL;
    PLPC_SHARED_MEMORY sharedMemory = NULL;
    SIZE_T viewSize = 0;
    size_t totalLength = 0;
    uint8_t actualAsync = 0;
    uint8_t actualShared = 0;
    void *sharedReplyCopy = NULL;
    uint32_t callbackLength = 0;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    requestBuffer = requestStorage.bytes;
    replyBuffer = replyStorage.bytes;

    if (!context || (!message && messageLength) ||
        lpc_state_load(&context->initialized) == 0 ||
        !context->hLPCPortHandle ||
        !context->api.pfnNtRequestPort || !context->api.pfnNtRequestWaitReplyPort) {
        return LPC_STATUS(STATUS_INVALID_PARAMETER);
    }
    if (!lpc_lifetime_acquire(&context->lifetime)) {
        return STATUS_DEVICE_BUSY;
    }
    lpc_lock_acquire(&context->sendLock);
    if (useAsyncMode && useSharedMemory) {
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }
    viewSize = context->client_view.ViewSize;
    actualAsync = useAsyncMode ? 1U : 0U;
    actualShared = useSharedMemory ? 1U : 0U;
    if (messageLength > LPC_MESSAGE_MAX_PACK_SIZE) {
        /* A datagram cannot safely reference section-backed data. */
        if (actualAsync) {
            status = STATUS_NOT_SUPPORTED;
            goto done;
        }
        /* Large messages use a synchronous section-backed exchange. */
        actualShared = 1;
    }

    libc_memset(requestBuffer, 0, sizeof(requestStorage));
    libc_memset(replyBuffer, 0, sizeof(replyStorage));
    if (actualShared) {
        if (!context->client_view.ViewBase || viewSize < sizeof(ULONG) ||
            (SIZE_T)messageLength > viewSize - sizeof(ULONG)) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            goto done;
        }
        sharedMemory = (PLPC_SHARED_MEMORY)context->client_view.ViewBase;
        sharedMemory->size = messageLength;
        if (messageLength) {
            libc_memcpy(sharedMemory->msg, message, messageLength);
        }
        libc_memset(&sharedHeader, 0, sizeof(sharedHeader));
        sharedHeader.ControlId = controlId;
        sharedHeader.u1.s1.ulUseSharedMemory = 1;
        sharedHeader.u1.s1.ulUseAsyncMethod = 0;
        lpc_init_port_message(&sharedHeader.header, LPC_HEADER_DATA_OFFSET,
                              (USHORT)LPC_TYPE_REQUEST);
        status = context->api.pfnNtRequestWaitReplyPort(context->hLPCPortHandle,
                                                         &sharedHeader.header,
                                                         &((PLPC_HEADER)replyBuffer)->header);
    } else {
        totalLength = LPC_HEADER_DATA_OFFSET + sizeof(ULONG) + messageLength;
        if (totalLength > LPC_PORT_NATIVE_MAX_MESSAGE_LENGTH ||
            totalLength > sizeof(requestStorage)) {
            status = STATUS_INFO_LENGTH_MISMATCH;
            goto done;
        }
        request = (PLPC_HEADER)(void *)requestBuffer;
        request->ControlId = controlId;
        request->u1.s1.ulUseSharedMemory = 0;
        request->u1.s1.ulUseAsyncMethod = actualAsync != 0U;
        inlineMessage = (PLPC_MESSAGE)(void *)request->data;
        inlineMessage->size = messageLength;
        if (messageLength) {
            libc_memcpy(inlineMessage->msg, message, messageLength);
        }
        lpc_init_port_message(&request->header, totalLength,
                              (USHORT)(actualAsync ? LPC_TYPE_DATAGRAM : LPC_TYPE_REQUEST));
        if (actualAsync) {
            status = context->api.pfnNtRequestPort(context->hLPCPortHandle, &request->header);
            goto done;
        }
        status = context->api.pfnNtRequestWaitReplyPort(context->hLPCPortHandle,
                                                        &request->header,
                                                        &((PLPC_HEADER)replyBuffer)->header);
    }
    /* A reply callback is optional, but wire validation is not.  Even callers
     * that only need the transport status must not accept a malformed reply;
     * otherwise a peer can silently desynchronize the next section-backed
     * transaction. */
    if (!NT_SUCCESS(status) || actualAsync) {
        goto done;
    }

    reply = (PLPC_HEADER)(void *)replyBuffer;
    if (actualShared) {
        uint32_t replyLength = 0;
        uint8_t *replyData = NULL;
        if (!lpc_port_message_valid(&reply->header, sizeof(replyStorage), LPC_HEADER_DATA_OFFSET) ||
            reply->header.u2.s2.Type != (USHORT)LPC_TYPE_REPLY ||
            reply->u1.s1.ulUseSharedMemory != actualShared ||
            reply->u1.s1.ulUseAsyncMethod || reply->u1.s1.ulReserved ||
            !lpc_shared_message((PLPC_SHARED_MEMORY)context->client_view.ViewBase, viewSize,
                                &replyLength, &replyData)) {
            status = STATUS_DATA_ERROR;
            goto done;
        }
        callbackLength = replyLength;
        if (callbackLength && replyCallback) {
            sharedReplyCopy = lpc_alloc_memory(callbackLength);
            if (!sharedReplyCopy) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                goto done;
            }
            libc_memcpy(sharedReplyCopy, replyData, callbackLength);
        }
    } else {
        PLPC_MESSAGE replyMessage = NULL;
        if (!lpc_port_message_valid(&reply->header, sizeof(replyStorage),
                                    LPC_HEADER_DATA_OFFSET + sizeof(ULONG)) ||
            reply->header.u2.s2.Type != (USHORT)LPC_TYPE_REPLY ||
            reply->u1.s1.ulUseSharedMemory != actualShared ||
            reply->u1.s1.ulUseAsyncMethod || reply->u1.s1.ulReserved ||
            !lpc_inline_message(reply, sizeof(replyStorage), &replyMessage, NULL)) {
            status = STATUS_DATA_ERROR;
            goto done;
        }
        callbackLength = replyMessage->size;
        if (callbackLength && replyCallback) {
            sharedReplyCopy = lpc_alloc_memory(callbackLength);
            if (!sharedReplyCopy) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                goto done;
            }
            libc_memcpy(sharedReplyCopy, replyMessage->msg, callbackLength);
        }
    }

done:
    lpc_lock_release(&context->sendLock);
    lpc_lifetime_release(&context->lifetime);
    if (NT_SUCCESS(status) && sharedReplyCopy && replyCallback) {
        replyCallback(sharedReplyCopy, callbackLength, callbackContext);
    } else if (NT_SUCCESS(status) && replyCallback && callbackLength == 0) {
        replyCallback(NULL, 0, callbackContext);
    }
    lpc_free_memory(sharedReplyCopy);
    return status;
}

uint8_t LpcPort_Register_ServerEvtCallback(PLPC_SERVER_CONTEXT context,
                                                  const LPC_SERVER_EVT_CONTEXT *events) {
    if (!context || !events || lpc_state_load(&context->initialized) == 0) {
        return 0;
    }
    if (!lpc_lifetime_acquire(&context->lifetime)) {
        return 0;
    }
    lpc_lock_acquire(&context->lockWord);
    context->ServerEvtCallback = *events;
    lpc_lock_release(&context->lockWord);
    lpc_lifetime_release(&context->lifetime);
    return 1;
}

NTSTATUS LpcPort_ProcessBlockedEventEx(PLPC_SERVER_CONTEXT context,
                                       const LARGE_INTEGER *timeout) {
    LPC_MESSAGE_BUFFER receiveStorage = {0};
    PLPC_HEADER received = NULL;
    PVOID portContext = NULL;
    HANDLE clientHandle = NULL;
    LPC_SERVER_CLIENT_INFO *clientInfo = NULL;
    LPC_SERVER_EVT_CONTEXT events = {0};
    USHORT messageType = 0;
    LARGE_INTEGER timeoutValue = {0};
    PLARGE_INTEGER timeoutArgument = NULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    NTSTATUS result = STATUS_UNSUCCESSFUL;
    uint8_t clientHeld = 0;
    uint8_t clientLockHeld = 0;

    if (!context || lpc_state_load(&context->initialized) == 0 ||
        !context->hLPCPortServerHandle) {
        return LPC_STATUS(STATUS_INVALID_PARAMETER);
    }
    if (!lpc_lifetime_acquire(&context->lifetime)) {
        return STATUS_DEVICE_BUSY;
    }
    if (timeout && !context->api.pfnNtReplyWaitReceivePortEx) {
        result = STATUS_NOT_SUPPORTED;
        goto done;
    }
    if (!timeout && !context->api.pfnNtReplyWaitReceivePort) {
        /* NtReplyWaitReceivePortEx has a mandatory timeout pointer. */
        result = STATUS_NOT_SUPPORTED;
        goto done;
    }
    libc_memset(&receiveStorage, 0, sizeof(receiveStorage));
    received = (PLPC_HEADER)(void *)receiveStorage.bytes;
    if (timeout) {
        timeoutValue = *timeout;
        timeoutArgument = &timeoutValue;
        status = context->api.pfnNtReplyWaitReceivePortEx(
            context->hLPCPortServerHandle, &portContext, NULL,
            &received->header, timeoutArgument);
    } else {
        status = context->api.pfnNtReplyWaitReceivePort(
            context->hLPCPortServerHandle, &portContext, NULL,
            &received->header);
    }
    if (status == STATUS_TIMEOUT || !NT_SUCCESS(status)) {
        result = status;
        goto done;
    }
    if (!lpc_port_message_valid(&received->header, sizeof(receiveStorage),
                                sizeof(LPC_PORT_MESSAGE))) {
        if (received->header.u2.s2.Type == (USHORT)LPC_TYPE_CONNECTION_REQUEST) {
            (void)lpc_reject_connection(context, &received->header);
        } else if (portContext &&
                   received->header.u2.s2.Type == (USHORT)LPC_TYPE_REQUEST &&
                   !received->u1.s1.ulUseAsyncMethod) {
            /* The native header can still identify the communication port
             * even when the SDK envelope length/checksum is malformed.  Send
             * a minimal reply for synchronous traffic so a client waiting in
             * NtRequestWaitReplyPort is not stranded forever. */
            clientInfo = lpc_acquire_client_by_context(context, portContext);
            if (clientInfo) {
                clientHeld = 1;
                lpc_lock_acquire(&clientInfo->receiveLock);
                clientLockHeld = 1;
                if (lpc_prepare_empty_reply(received,
                                             sizeof(receiveStorage))) {
                    if (received->u1.s1.ulUseSharedMemory) {
                        lpc_clear_shared_memory(clientInfo);
                    }
                    result = context->api.pfnNtReplyPort(
                        clientInfo->hLPCPortClientHandle,
                        &received->header);
                } else {
                    result = STATUS_DATA_ERROR;
                }
            } else {
                result = STATUS_PORT_DISCONNECTED;
            }
        } else {
            result = STATUS_DATA_ERROR;
        }
        goto done;
    }
    messageType = received->header.u2.s2.Type;
    if (messageType == LPC_TYPE_CONNECTION_REQUEST) {
        uint8_t deny = 1;
        uint32_t responseControlId = 0;
        LPC_REMOTE_PORT_VIEW remoteView = {0};
        PLPC_SERVER_CLIENT_INFO pending = NULL;

        /* The server receives only the caller's four-byte connection info. */
        if (received->header.u1.s1.TotalLength !=
            sizeof(LPC_PORT_MESSAGE) + sizeof(uint32_t)) {
            (void)lpc_reject_connection(context, &received->header);
            result = STATUS_DATA_ERROR;
            goto done;
        }
        responseControlId = (uint32_t)received->ControlId;
        lpc_snapshot_events(context, &events);
        if (events.onPreConnect) {
            events.onPreConnect(&responseControlId, &deny);
        }
        received->ControlId = (ULONG)responseControlId;
        /* PortContext is returned on every later receive.  Allocate the
         * record before accepting so the kernel can retain this pointer. */
        if (!deny) {
            pending = (PLPC_SERVER_CLIENT_INFO)lpc_alloc_memory(sizeof(*pending));
            if (!pending) {
                (void)lpc_reject_connection(context, &received->header);
                result = STATUS_INSUFFICIENT_RESOURCES;
                goto done;
            }
            libc_memset(pending, 0, sizeof(*pending));
            if (!lpc_lifetime_init(&pending->lifetime)) {
                lpc_free_memory(pending);
                (void)lpc_reject_connection(context, &received->header);
                result = STATUS_INSUFFICIENT_RESOURCES;
                goto done;
            }
        }
        libc_memset(&remoteView, 0, sizeof(remoteView));
        remoteView.Length = (ULONG)sizeof(remoteView);
        status = context->api.pfnNtAcceptConnectPort(
            &clientHandle, (PVOID)pending, &received->header, (BOOLEAN)!deny,
            NULL, deny ? NULL : &remoteView);
        if (!NT_SUCCESS(status)) {
            if (pending) {
                lpc_close_client_info(pending);
            }
            result = status;
            goto done;
        }
        if (deny) {
            if (clientHandle && context->api.pfnNtClose) {
                context->api.pfnNtClose(clientHandle);
            }
            if (pending) {
                lpc_close_client_info(pending);
            }
            result = STATUS_SUCCESS;
            goto done;
        }
        if (!clientHandle) {
            if (pending) {
                lpc_close_client_info(pending);
            }
            result = STATUS_DATA_ERROR;
            goto done;
        }
        status = context->api.pfnNtCompleteConnectPort(clientHandle);
        if (!NT_SUCCESS(status)) {
            context->api.pfnNtClose(clientHandle);
            if (pending) {
                lpc_close_client_info(pending);
            }
            result = status;
            goto done;
        }
        /* The accepted connection must have a stable PortContext record.  Keep
         * this guard even though the allocation path above normally proves it
         * non-NULL; it protects future changes and malformed test doubles. */
        if (!pending) {
            if (context->api.pfnNtClose) {
                context->api.pfnNtClose(clientHandle);
            }
            result = STATUS_INSUFFICIENT_RESOURCES;
            goto done;
        }
        clientInfo = pending;
        clientInfo->hLPCPortClientHandle = clientHandle;
        clientInfo->client_view = remoteView;
        clientInfo->controlId = responseControlId;
        if (!lpc_add_client(context, clientInfo)) {
            lpc_close_client_info(clientInfo);
            context->api.pfnNtClose(clientHandle);
            clientInfo = NULL;
            result = STATUS_DEVICE_BUSY;
            goto done;
        }
        if (events.onPostConnect) {
            events.onPostConnect(clientHandle, received->ControlId);
        }
        result = STATUS_SUCCESS;
        goto done;
    }

    /* A non-connection message must belong to a tracked client. */
    if (!portContext) {
        result = STATUS_PORT_DISCONNECTED;
        goto done;
    }
    clientInfo = lpc_acquire_client_by_context(context, portContext);
    if (!clientInfo) {
        result = STATUS_PORT_DISCONNECTED;
        goto done;
    }
    clientHeld = 1;
    clientHandle = clientInfo->hLPCPortClientHandle;
    lpc_lock_acquire(&clientInfo->receiveLock);
    clientLockHeld = 1;
    lpc_snapshot_events(context, &events);

    /* A request with a truncated envelope is still attributable to this
     * client through PortContext.  Return an empty reply before reporting the
     * validation error so the sender cannot wait indefinitely. */
    /* Shared-memory requests carry no inline LPC_MESSAGE size field; their
     * valid envelope ends at LPC_HEADER_DATA_OFFSET.  Only inline requests
     * require the additional ULONG before they can be parsed. */
    if (received->header.u1.s1.TotalLength < LPC_HEADER_DATA_OFFSET ||
        (!received->u1.s1.ulUseSharedMemory &&
         received->header.u1.s1.TotalLength <
             LPC_HEADER_DATA_OFFSET + sizeof(ULONG))) {
        if (lpc_prepare_empty_reply(received, sizeof(receiveStorage))) {
            if (received->u1.s1.ulUseSharedMemory) {
                lpc_clear_shared_memory(clientInfo);
            }
            result = context->api.pfnNtReplyPort(
                clientInfo->hLPCPortClientHandle, &received->header);
        } else {
            result = STATUS_DATA_ERROR;
        }
        goto done;
    }

    switch (messageType) {
        case LPC_TYPE_REQUEST: {
            uint8_t validPayload = 0;
            uint8_t *callbackData = NULL;
            uint32_t callbackLength = 0;
            uint8_t *sharedData = NULL;
            uint32_t sharedLength = 0;

            if (received->u1.s1.ulUseAsyncMethod || received->u1.s1.ulReserved) {
                if (lpc_prepare_empty_reply(received, sizeof(receiveStorage))) {
                    if (received->u1.s1.ulUseSharedMemory) {
                        lpc_clear_shared_memory(clientInfo);
                    }
                    result = context->api.pfnNtReplyPort(
                        clientInfo->hLPCPortClientHandle, &received->header);
                } else {
                    result = STATUS_DATA_ERROR;
                }
                break;
            }
            if (received->u1.s1.ulUseSharedMemory) {
                if (lpc_shared_message((PLPC_SHARED_MEMORY)clientInfo->client_view.ViewBase,
                                       clientInfo->client_view.ViewSize,
                                       &sharedLength, &sharedData) &&
                    sharedLength <= (uint32_t)clientInfo->client_view.ViewSize) {
                    callbackLength = sharedLength;
                    if (sharedLength) {
                        callbackData = (uint8_t *)lpc_alloc_memory(sharedLength);
                    }
                    if (!sharedLength || callbackData) {
                        if (sharedLength) {
                            libc_memcpy(callbackData, sharedData, sharedLength);
                        }
                        validPayload = 1;
                    }
                }
            } else {
                PLPC_MESSAGE inlineMessage = NULL;
                if (lpc_inline_message(received, sizeof(receiveStorage),
                                       &inlineMessage, NULL) &&
                    inlineMessage->size + sizeof(ULONG) ==
                        received->header.u1.s1.TotalLength - LPC_HEADER_DATA_OFFSET) {
                    callbackLength = inlineMessage->size;
                    callbackData = inlineMessage->msg;
                    validPayload = 1;
                }
            }
            if (validPayload && events.onSyncRequest) {
                events.onSyncRequest(clientHandle, received->ControlId,
                                     callbackData, callbackLength,
                                     received->u1.s1.ulUseSharedMemory
                                         ? (uint64_t)clientInfo->client_view.ViewSize
                                         : (uint64_t)LPC_MESSAGE_MAX_PACK_SIZE);
                if (received->u1.s1.ulUseSharedMemory && callbackLength) {
                    libc_memcpy(sharedData, callbackData, callbackLength);
                }
            }
            if (received->u1.s1.ulUseSharedMemory && callbackData &&
                callbackData != sharedData) {
                lpc_free_memory(callbackData);
            }
            if (!validPayload) {
                if (lpc_prepare_empty_reply(received, sizeof(receiveStorage))) {
                    if (received->u1.s1.ulUseSharedMemory) {
                        lpc_clear_shared_memory(clientInfo);
                    }
                    result = context->api.pfnNtReplyPort(
                        clientInfo->hLPCPortClientHandle, &received->header);
                } else {
                    result = STATUS_DATA_ERROR;
                }
                break;
            }
            received->header.u2.s2.Type = (USHORT)LPC_TYPE_REPLY;
            /* Replies must use the accepted communication-port handle, not
             * the connection-port handle on which the receive was issued. */
            result = context->api.pfnNtReplyPort(
                clientInfo->hLPCPortClientHandle, &received->header);
            break;
        }

        case LPC_TYPE_DATAGRAM: {
            PLPC_MESSAGE inlineMessage = NULL;
            if (received->u1.s1.ulUseSharedMemory ||
                received->u1.s1.ulReserved ||
                !lpc_inline_message(received, sizeof(receiveStorage),
                                    &inlineMessage, NULL) ||
                inlineMessage->size + sizeof(ULONG) !=
                    received->header.u1.s1.TotalLength - LPC_HEADER_DATA_OFFSET) {
                result = STATUS_DATA_ERROR;
                break;
            }
            if (events.onAsyncRequest) {
                events.onAsyncRequest(clientHandle, received->ControlId,
                                      inlineMessage->msg, inlineMessage->size,
                                      (uint64_t)LPC_MESSAGE_MAX_PACK_SIZE);
            }
            result = STATUS_SUCCESS;
            break;
        }

        case LPC_TYPE_PORT_CLOSED:
            /* Defer detach/notification until the common epilogue.  The
             * current record pin must be released first; lpc_drop_client
             * drains the remaining pins and therefore cannot wait on itself. */
            lpc_lock_release(&clientInfo->receiveLock);
            clientLockHeld = 0;
            lpc_lifetime_release(&clientInfo->lifetime);
            clientHeld = 0;
            (void)lpc_drop_client(context, clientHandle, 1);
            result = STATUS_SUCCESS;
            break;

        default:
            /* A synchronous peer can otherwise remain blocked forever when a
             * new/unknown type reaches an older server.  Datagram-style
             * messages have no reply channel and are simply rejected. */
            if (!received->u1.s1.ulUseAsyncMethod &&
                lpc_prepare_empty_reply(received, sizeof(receiveStorage))) {
                if (received->u1.s1.ulUseSharedMemory) {
                    lpc_clear_shared_memory(clientInfo);
                }
                result = context->api.pfnNtReplyPort(
                    clientInfo->hLPCPortClientHandle, &received->header);
            } else {
                result = STATUS_DATA_ERROR;
            }
            break;
    }

done:
    if (clientLockHeld && clientInfo) {
        lpc_lock_release(&clientInfo->receiveLock);
    }
    if (clientHeld && clientInfo) {
        lpc_lifetime_release(&clientInfo->lifetime);
    }
    lpc_lifetime_release(&context->lifetime);
    return result;
}

NTSTATUS LpcPort_ProcessBlockedEvent(PLPC_SERVER_CONTEXT context) {
    return LpcPort_ProcessBlockedEventEx(context, NULL);
}
