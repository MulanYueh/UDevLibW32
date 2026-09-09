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
    return 1;
}

static int alpc_lifetime_acquire(ALPC_PORT_LIFETIME *life)
{
    if (!life || !life->initialized) {
        return 0;
    }
#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    {
        int acquired = 0;
        alpc_lifetime_admission_lock(life);
        if (life->initialized && life->closing == 0 &&
            ExAcquireRundownProtection(&life->rundown)) {
            acquired = 1;
        }
        alpc_lifetime_admission_unlock(life);
        return acquired;
    }
#elif !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
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
        alpc_lifetime_fallback_lock(life);
        if (life->initialized && !life->closing) {
            ++life->active;
            acquired = 1;
        }
        alpc_lifetime_fallback_unlock(life);
        return acquired;
    }
#endif
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
    if (!life || !life->initialized) {
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
#if !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
    if (life->initialized) {
        DeleteCriticalSection(&life->native);
    }
#endif
    life->initialized = 0;
}

static PALPC_PORT_SERVER_CLIENT alpc_find_client_locked(
    PALPC_PORT_SERVER_CONTEXT context, HANDLE clientPort);

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

/* A native ALPC connection port is also the receive queue for messages from
 * accepted clients.  A dispatcher that waits on that shared handle does not
 * know the communication handle until it inspects the message, so provide a
 * safe single-client fallback for that receive path.  Multi-client servers
 * should use the ClientId in their dispatcher to select the record. */
static PALPC_PORT_SERVER_CLIENT alpc_acquire_first_client(
    PALPC_PORT_SERVER_CONTEXT context)
{
    PALPC_PORT_SERVER_CLIENT current = NULL;

    if (!context || !context->lockWord.initialized) {
        return NULL;
    }
    alpc_lock_acquire(&context->lockWord);
    current = (PALPC_PORT_SERVER_CLIENT)List_Head(&context->clientList);
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
    frame->header.u2.s2.Type = (USHORT)ALPC_PORT_MESSAGE_TYPE_REPLY;
    frame->Flags = 0;
    frame->PayloadLength = 0;
    frame->Status = errorStatus;
    return alpc_set_frame_length(frame, capacity, 0);
}

static NTSTATUS alpc_send_error_reply(const ALPC_PORT_SERVER_CONTEXT *context,
                                      HANDLE clientPort,
                                      PALPC_PORT_FRAME frame,
                                      NTSTATUS errorStatus)
{
    NTSTATUS sendStatus = STATUS_DATA_ERROR;
    ALPC_PORT_MESSAGE receiveHeader = ALPC_PORT_ZERO_INIT;
    SIZE_T receiveLength = sizeof(receiveHeader);
    LARGE_INTEGER noWait = ALPC_PORT_ZERO_INIT;

    if (!context || !context->api.pfnNtAlpcSendWaitReceivePort ||
        !alpc_prepare_error_reply(frame, context->maxMessageLength,
                                  errorStatus)) {
        return STATUS_DATA_ERROR;
    }
    sendStatus = context->api.pfnNtAlpcSendWaitReceivePort(
        clientPort, ALPC_PORT_SEND_FLAG_REPLY_MESSAGE, &frame->header, NULL,
        &receiveHeader, &receiveLength, NULL, &noWait);
    if (sendStatus == STATUS_TIMEOUT) {
        sendStatus = STATUS_SUCCESS;
    }
    return NT_SUCCESS(sendStatus) ? errorStatus : sendStatus;
}

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

/* Native ALPC writes the number of bytes actually received to BufferLength.
 * Validate that value independently from the allocation capacity; otherwise
 * a short/malformed message could make the parser consume zero-filled bytes
 * that were never supplied by the peer. */
static int alpc_received_frame_valid(const PALPC_PORT_FRAME frame,
                                     SIZE_T receivedLength,
                                     SIZE_T capacity)
{
    if (receivedLength < (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET ||
        receivedLength > capacity) {
        return 0;
    }
    return alpc_frame_valid(frame, receivedLength);
}

static int alpc_message_type_is(USHORT actual, ULONG expected)
{
    return actual == (USHORT)expected || actual == (USHORT)(expected & 0x0fffU);
}

/* Native ALPC control notifications are not SDK frames.  Depending on the
 * Windows build, the Type field is reported either with the ALPC 0x2000
 * namespace or as the low 12-bit LPC value.  Keep this test in one place so
 * normal endpoint shutdown is never mistaken for corrupt application data. */
static int alpc_native_control_type(USHORT actual)
{
    return alpc_message_type_is(actual, ALPC_PORT_MESSAGE_TYPE_PORT_CLOSED) ||
           actual == (USHORT)0x000BU || actual == (USHORT)0x000CU ||
           actual == (USHORT)(ALPC_PORT_MESSAGE_TYPE_CONNECTION_REQUEST + 1U);
}

static int alpc_native_connection_complete_type(USHORT actual)
{
    return actual == (USHORT)0x000BU ||
           actual == (USHORT)(ALPC_PORT_MESSAGE_TYPE_CONNECTION_REQUEST + 1U);
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

static int alpc_add_client(PALPC_PORT_SERVER_CONTEXT context,
                           PALPC_PORT_SERVER_CLIENT client)
{
    int inserted = 0;

    if (!context || !client) {
        return 0;
    }
    if (!client->receiveLock.initialized && !alpc_lock_init(&client->receiveLock)) {
        return 0;
    }
    if (!client->lifetime.initialized && !alpc_lifetime_init(&client->lifetime)) {
        alpc_lock_destroy(&client->receiveLock);
        return 0;
    }
    alpc_lock_acquire(&context->lockWord);
    if ((!context->maxClients || context->clientCount < context->maxClients) &&
        !alpc_find_client_locked(context, client->portHandle) &&
        List_Insert_After(&context->clientList, List_Tail(&context->clientList),
                          &client->listEntry)) {
        if (context->clientCount != (uint32_t)-1) {
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
        !List_Init(&context->clientList) || !alpc_lock_init(&context->lockWord)) {
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_server_context(context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = alpc_build_unicode_name(context->api.pfnRtlInitAnsiString,
                                      context->api.pfnRtlAnsiStringToUnicodeString,
                                      &config->portName, &context->unicodeName);
    if (!NT_SUCCESS(status)) {
        alpc_lock_destroy(&context->lockWord);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_server_context(context);
        return status;
    }
    alpc_init_object_attributes(&objectAttributes, &context->unicodeName,
                                config->securityDescriptor);
    securityQos = config->securityQos;
    alpc_init_qos(&securityQos);
    alpc_init_port_attributes(&portAttributes, maxMessageLength, config->portFlags,
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
        alpc_lock_destroy(&context->lockWord);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_server_context(context);
        return status;
    }
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
    bufferLength = context->maxMessageLength;
    if (timeout) {
        timeoutValue = *timeout;
        timeoutArgument = &timeoutValue;
    }
    status = context->api.pfnNtAlpcSendWaitReceivePort(
        context->connectionPortHandle, 0, NULL, NULL, &frame->header,
        &bufferLength, NULL, timeoutArgument);
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

        /* PortContext is the stable record pointer returned by subsequent
         * NtReplyWaitReceivePort calls.  Allocate it before accept so the
         * native endpoint can retain that value without a handle->context map. */
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
        }
        status = context->api.pfnNtAlpcAcceptConnectPort(
            &clientPort, context->connectionPortHandle, 0, NULL, NULL,
            (PVOID)pending,
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
    ALPC_PORT_MESSAGE receiveHeader = ALPC_PORT_ZERO_INIT;
    SIZE_T receiveLength = sizeof(receiveHeader);
    LARGE_INTEGER noWait = ALPC_PORT_ZERO_INIT;

    if (!context || alpc_state_load(&context->initialized) == 0 || !clientPort ||
        !alpc_lifetime_acquire(&context->lifetime)) {
        return ALPC_PORT_STATUS(STATUS_INVALID_PARAMETER);
    }
    client = alpc_acquire_client(context, clientPort);
    if (!client && clientPort == context->connectionPortHandle) {
        client = alpc_acquire_first_client(context);
    }
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
                                            STATUS_DATA_ERROR);
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
                                           STATUS_DATA_ERROR);
        } else {
            result = STATUS_DATA_ERROR;
        }
        alpc_free(frame);
        goto done;
    }
    if (frame->Flags != 0 || frame->Status != STATUS_SUCCESS) {
        result = alpc_send_error_reply(context, replyPort, frame,
                                       STATUS_DATA_ERROR);
        alpc_free(frame);
        goto done;
    }
    alpc_snapshot_events(context, &events);
    replyLength = frame->PayloadLength;
    callbackStatus = STATUS_SUCCESS;
    if (events.onSyncRequest) {
        callbackStatus = events.onSyncRequest(
            replyPort, &frame->header.ClientId, frame->ControlId, frame->data,
            &replyLength,
            (ULONG)(context->maxMessageLength - (SIZE_T)ALPC_PORT_FRAME_DATA_OFFSET),
            events.callbackContext);
    }
    if (!alpc_set_frame_length(frame, context->maxMessageLength, replyLength)) {
        result = alpc_send_error_reply(context, replyPort, frame,
                                       STATUS_INFO_LENGTH_MISMATCH);
        alpc_free(frame);
        goto done;
    }
    frame->header.u2.s2.Type = (USHORT)ALPC_PORT_MESSAGE_TYPE_REPLY;
    frame->Flags = 0;
    frame->Status = callbackStatus;
    sendStatus = context->api.pfnNtAlpcSendWaitReceivePort(
        replyPort, ALPC_PORT_SEND_FLAG_REPLY_MESSAGE,
        &frame->header, NULL,
        &receiveHeader, &receiveLength, NULL, &noWait);
    if (sendStatus == STATUS_TIMEOUT) {
        sendStatus = STATUS_SUCCESS;
    }
    alpc_free(frame);
    if (!NT_SUCCESS(sendStatus)) {
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
        (void)alpc_drop_client(context, replyPort, 0, 1);
    }
    alpc_lifetime_release(&context->lifetime);
    return result;
}

NTSTATUS AlpcPort_ProcessClientEvent(PALPC_PORT_SERVER_CONTEXT context,
                                      HANDLE clientPort)
{
    return AlpcPort_ProcessClientEventEx(context, clientPort, NULL);
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

NTSTATUS AlpcPort_Connect(PALPC_PORT_CLIENT_CONFIG config,
                          PALPC_PORT_CLIENT_CONTEXT context)
{
    ALPC_PORT_SDK_ATTRIBUTES portAttributes = ALPC_PORT_ZERO_INIT;
    SECURITY_QUALITY_OF_SERVICE securityQos = ALPC_PORT_ZERO_INIT;
    PALPC_PORT_FRAME connectionFrame = NULL;
    SIZE_T bufferLength = 0;
    SIZE_T maxMessageLength = 0;
    LARGE_INTEGER timeoutValue = ALPC_PORT_ZERO_INIT;
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
    alpc_reset_client_context(context);
    context->api = config->api;
    if (!alpc_lifetime_init(&context->lifetime) ||
        !alpc_lock_init(&context->sendLock)) {
        alpc_lifetime_destroy(&context->lifetime);
        alpc_lock_destroy(&context->sendLock);
        alpc_reset_client_context(context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = alpc_build_unicode_name(context->api.pfnRtlInitAnsiString,
                                      context->api.pfnRtlAnsiStringToUnicodeString,
                                      &config->portName, &context->unicodeName);
    if (!NT_SUCCESS(status)) {
        alpc_lock_destroy(&context->sendLock);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_client_context(context);
        return status;
    }
    connectionFrame = (PALPC_PORT_FRAME)alpc_alloc(maxMessageLength);
    if (!connectionFrame) {
        context->api.pfnRtlFreeUnicodeString(&context->unicodeName);
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
        alpc_lock_destroy(&context->sendLock);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_client_context(context);
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    securityQos = config->securityQos;
    alpc_init_qos(&securityQos);
    alpc_init_port_attributes(&portAttributes, maxMessageLength, config->portFlags,
                              0, 0, 0, 0, &securityQos);
    bufferLength = maxMessageLength;
    if (config->connectTimeout) {
        timeoutValue = *config->connectTimeout;
        timeoutArgument = &timeoutValue;
    }
    status = context->api.pfnNtAlpcConnectPort(
        &context->portHandle, &context->unicodeName, NULL, &portAttributes, 0,
        NULL, &connectionFrame->header, &bufferLength, NULL, NULL,
        timeoutArgument);
    if (!NT_SUCCESS(status) || !context->portHandle) {
        if (NT_SUCCESS(status)) {
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
        alpc_lock_destroy(&context->sendLock);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_client_context(context);
        return STATUS_DATA_ERROR;
    }
    context->maxMessageLength = maxMessageLength;
    context->negotiatedControlId = connectionFrame->ControlId;
    /* Some Windows builds queue a native type-11 connection-complete marker
     * on the client endpoint after NtAlpcConnectPort returns.  Poll it now,
     * before the first application request; otherwise that marker can be
     * returned as the apparent reply to that request. */
    libc_memset(connectionFrame, 0, (size_t)maxMessageLength);
    bufferLength = maxMessageLength;
    timeoutValue.QuadPart = 0;
    status = context->api.pfnNtAlpcSendWaitReceivePort(
        context->portHandle, 0, NULL, NULL, &connectionFrame->header,
        &bufferLength, NULL, &timeoutValue);
    if (NT_SUCCESS(status) && connectionFrame->header.u2.s2.Type != 0U &&
        !alpc_native_connection_complete_type(connectionFrame->header.u2.s2.Type)) {
        /* Do not silently consume an application message returned during
         * setup; reject the connection rather than desynchronizing it. */
        if (context->api.pfnNtAlpcDisconnectPort) {
            (void)context->api.pfnNtAlpcDisconnectPort(context->portHandle, 0);
        }
        if (context->api.pfnNtClose) {
            (void)context->api.pfnNtClose(context->portHandle);
        }
        alpc_free(connectionFrame);
        context->api.pfnRtlFreeUnicodeString(&context->unicodeName);
        alpc_lock_destroy(&context->sendLock);
        alpc_lifetime_destroy(&context->lifetime);
        alpc_reset_client_context(context);
        return STATUS_DATA_ERROR;
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
    uint32_t receiveAttempts = 0;
    LARGE_INTEGER timeoutValue = ALPC_PORT_ZERO_INIT;
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
    alpc_lock_acquire(&context->sendLock);
    if (timeout) {
        timeoutValue = *timeout;
        timeoutArgument = &timeoutValue;
    }
    asynchronous = (flags & ALPC_PORT_SEND_FLAG_ASYNC) ? 1U : 0U;
    requestFrame = (PALPC_PORT_FRAME)alpc_alloc(context->maxMessageLength);
    if (!requestFrame) {
        result = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    if (!alpc_init_frame(requestFrame, context->maxMessageLength,
                         (USHORT)(asynchronous ? ALPC_PORT_MESSAGE_TYPE_DATAGRAM
                                               : ALPC_PORT_MESSAGE_TYPE_REQUEST),
                         controlId, flags, message, messageLength)) {
        alpc_free(requestFrame);
        result = STATUS_INFO_LENGTH_MISMATCH;
        goto done;
    }
    if (asynchronous) {
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
    status = context->api.pfnNtAlpcSendWaitReceivePort(
        context->portHandle, ALPC_PORT_NATIVE_FLAG_SYNC_REQUEST,
        &requestFrame->header, NULL, &replyFrame->header,
        &replyLength, NULL, timeoutArgument);
    if (NT_SUCCESS(status) &&
        alpc_native_connection_complete_type(replyFrame->header.u2.s2.Type)) {
        for (receiveAttempts = 0; receiveAttempts < 4U; ++receiveAttempts) {
            libc_memset(replyFrame, 0, (size_t)context->maxMessageLength);
            replyLength = context->maxMessageLength;
            status = context->api.pfnNtAlpcSendWaitReceivePort(
                context->portHandle, 0, NULL, NULL, &replyFrame->header,
                &replyLength, NULL, timeoutArgument);
            if (!NT_SUCCESS(status) ||
                !alpc_native_connection_complete_type(
                    replyFrame->header.u2.s2.Type)) {
                break;
            }
        }
    }
    alpc_free(requestFrame);
    if (!NT_SUCCESS(status)) {
        alpc_free(replyFrame);
        result = status;
        goto done;
    }
    if (!alpc_received_frame_valid(replyFrame, replyLength,
                                   context->maxMessageLength) ||
        (!alpc_message_type_is(replyFrame->header.u2.s2.Type,
                               ALPC_PORT_MESSAGE_TYPE_REPLY) &&
         !alpc_message_type_is(replyFrame->header.u2.s2.Type,
                               ALPC_PORT_MESSAGE_TYPE_REQUEST)) ||
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
    result = NT_SUCCESS(remoteStatus) ? status : remoteStatus;

done:
    alpc_lock_release(&context->sendLock);
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
    alpc_lock_destroy(&context->sendLock);
    alpc_lifetime_destroy(&context->lifetime);
    alpc_reset_client_context(context);
}

void AlpcPort_Disconnect(PALPC_PORT_CLIENT_CONTEXT context)
{
    AlpcPort_DisConnect(context);
}
