/*
 * Native LPC transport shared by Windows user-mode and kernel-mode code.
 *
 * The original version of this header depended on a private include tree and
 * exposed its private list/allocator implementation.  This version keeps the
 * transport ABI small and self-contained.  The LPC wire structures are kept
 * at their native alignment; only the payload is application-defined.
 */
#ifndef LPC_PORT_H_INCLUDED
#define LPC_PORT_H_INCLUDED

#if !defined(OXDRV_KLIBC_H_INCLUDED) && !defined(_STDINT_H) && \
    !defined(_STDINT_H_) && !defined(_GCC_WRAP_STDINT_H)
#  if defined(LPC_PORT_USE_STDINT2) || \
      (defined(_MSC_VER) && !defined(__clang__) && !defined(__GNUC__))
#    if defined(__GNUC__) || defined(__clang__)
#      pragma GCC diagnostic push
#      pragma GCC diagnostic ignored "-Wunknown-pragmas"
#    endif
#    include "stdint2.h"
#    if defined(__GNUC__) || defined(__clang__)
#      pragma GCC diagnostic pop
#    endif
#  else
#    include <stdint.h>
#  endif
#endif
#include <stddef.h>

/* SDK/WDK headers provide the native scalar, handle and string types.  The
 * fallback declarations make the header usable by documentation generators
 * and by test harnesses that provide their own API table without Windows SDK. */
#if defined(_KERNEL_MODE)
#  if defined(_MSC_VER)
#    include <ntifs.h>
#    define LPC_PORT_HAS_PLATFORM_HEADERS 1
#  elif defined(__has_include)
#    if __has_include(<ntifs.h>)
#      include <ntifs.h>
#      define LPC_PORT_HAS_PLATFORM_HEADERS 1
#    endif
#  endif
#else
#  if defined(_MSC_VER)
#    include <windows.h>
#    include <winternl.h>
#    define LPC_PORT_HAS_PLATFORM_HEADERS 1
#  elif defined(__has_include)
#    if __has_include(<windows.h>)
#      include <windows.h>
#      include <winternl.h>
#      define LPC_PORT_HAS_PLATFORM_HEADERS 1
#    endif
#  endif
#endif

#ifndef LPC_PORT_HAS_PLATFORM_HEADERS
#  define LPC_PORT_HAS_PLATFORM_HEADERS 0
#  ifndef LPC_ALPC_PORT_COMMON_TYPES_DEFINED
#    define LPC_ALPC_PORT_COMMON_TYPES_DEFINED 1
typedef uint8_t UCHAR;
typedef uint8_t BOOLEAN;
typedef uint16_t USHORT;
/* WDK spells ULONG as unsigned long (32-bit on every Windows target).  Match
 * allocator.h's freestanding fallback so C++ callers may include either
 * header first without a conflicting typedef. */
typedef unsigned long ULONG;
typedef int32_t LONG;
typedef int64_t LONGLONG;
#if defined(_WIN64) || defined(_M_AMD64) || defined(__x86_64__) || \
    defined(_M_ARM64) || defined(__aarch64__)
typedef unsigned long long ULONG_PTR;
#else
typedef unsigned long ULONG_PTR;
#endif
typedef uint32_t ACCESS_MASK;
typedef void *PVOID;
typedef void *HANDLE;
typedef HANDLE *PHANDLE;
typedef char *PCHAR;
typedef const char *PCSZ;
typedef ULONG *PULONG;
typedef size_t SIZE_T;
typedef SIZE_T *PSIZE_T;
typedef struct _LPC_LARGE_INTEGER { LONGLONG QuadPart; } LARGE_INTEGER;
typedef LARGE_INTEGER *PLARGE_INTEGER;
typedef struct _LPC_ANSI_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
} ANSI_STRING, *PANSI_STRING;
typedef struct _LPC_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    wchar_t *Buffer;
} UNICODE_STRING, *PUNICODE_STRING;
typedef const UNICODE_STRING *PCUNICODE_STRING;
typedef struct _LPC_OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
typedef struct _LPC_SECURITY_QUALITY_OF_SERVICE {
    ULONG Length;
    ULONG ImpersonationLevel;
    BOOLEAN ContextTrackingMode;
    BOOLEAN EffectiveOnly;
} SECURITY_QUALITY_OF_SERVICE, *PSECURITY_QUALITY_OF_SERVICE;
typedef PVOID PSECURITY_DESCRIPTOR;
typedef LONG NTSTATUS;
#  endif /* LPC_ALPC_PORT_COMMON_TYPES_DEFINED */
#  ifndef NTAPI
#    define NTAPI
#  endif
#  ifndef OBJ_CASE_INSENSITIVE
#    define OBJ_CASE_INSENSITIVE 0x00000040UL
#  endif
#  ifndef SECTION_MAP_READ
#    define SECTION_MAP_READ 0x0004UL
#  endif
#  ifndef SECTION_MAP_WRITE
#    define SECTION_MAP_WRITE 0x0002UL
#  endif
#  ifndef PAGE_READWRITE
#    define PAGE_READWRITE 0x04UL
#  endif
#  ifndef SEC_COMMIT
#    define SEC_COMMIT 0x08000000UL
#  endif
#  ifndef SecurityImpersonation
#    define SecurityImpersonation 2UL
#  endif
#  ifndef SECURITY_DYNAMIC_TRACKING
#    define SECURITY_DYNAMIC_TRACKING 1
#  endif
#endif

#include "list.h"

/* LPC only holds this lock around short list/callback-snapshot operations and
 * never invokes callbacks or Native LPC APIs while it is held.  It is
 * intentionally non-recursive in kernel mode; the generic lock.h/c module is
 * reserved for callers that require recursive ERESOURCE semantics. */
#if defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
typedef struct _LPC_PORT_LOCK {
    EX_PUSH_LOCK native;
    uint8_t initialized;
} LPC_PORT_LOCK;
#elif !defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
typedef struct _LPC_PORT_LOCK {
    CRITICAL_SECTION native;
    uint8_t initialized;
} LPC_PORT_LOCK;
#else
typedef struct _LPC_PORT_LOCK {
    volatile uint32_t native;
    uint8_t initialized;
} LPC_PORT_LOCK;
#endif

/*
 * Lifetime protection is separate from the short list lock above.  A list
 * lock only makes a lookup atomic; it does not keep a handle or a context
 * alive after the lock is released.  The lifetime object closes that gap and
 * lets ServerClose/Disconnect wait for in-flight Native calls to finish.
 */
#if defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
typedef struct _LPC_PORT_LIFETIME {
    EX_RUNDOWN_REF rundown;
    volatile LONG admission;
    volatile LONG closing;
    uint8_t initialized;
} LPC_PORT_LIFETIME;
#elif !defined(_KERNEL_MODE) && LPC_PORT_HAS_PLATFORM_HEADERS
typedef struct _LPC_PORT_LIFETIME {
    CRITICAL_SECTION native;
    CONDITION_VARIABLE idle;
    uint32_t active;
    uint8_t closing;
    uint8_t initialized;
} LPC_PORT_LIFETIME;
#else
typedef struct _LPC_PORT_LIFETIME {
    volatile uint32_t native;
    volatile uint32_t active;
    uint8_t closing;
    uint8_t initialized;
} LPC_PORT_LIFETIME;
#endif

/* Export the private CRT declarations once so callers may include libc.h
 * after this header without reintroducing the platform stdint typedef clash. */
#if !defined(LPC_PORT_NO_LIBC_HEADER) && !defined(OXDRV_KLIBC_H_INCLUDED)
#  ifndef _STDINT_H
#    define _STDINT_H
#    define LPC_PORT_DEFINED_STDINT_GUARD 1
#  endif
#  if defined(_MSC_VER) && !defined(_STDINT_H_)
#    define _STDINT_H_
#    define LPC_PORT_DEFINED_STDINT_GUARD_MSVC 1
#  endif
#  include "libc.h"
#endif

/* Native LPC allows 648 bytes on 64-bit Windows and 328 bytes on 32-bit
 * Windows.  The transport header consumes 12 bytes in addition to the native
 * PORT_MESSAGE and the LPC_MESSAGE size field, so the inline payload is kept
 * below that architectural limit. */
#if defined(_WIN64) || defined(_M_AMD64) || defined(__x86_64__) || \
    defined(__amd64__) || defined(_M_ARM64) || defined(__aarch64__)
#  define LPC_PORT_NATIVE_MAX_MESSAGE_LENGTH 648U
#  define LPC_MESSAGE_MAX_PACK_SIZE 0x240U
#else
#  define LPC_PORT_NATIVE_MAX_MESSAGE_LENGTH 328U
#  define LPC_MESSAGE_MAX_PACK_SIZE 0x120U
#endif

#define LPC_PORT_BUFFER_SIZE LPC_PORT_NATIVE_MAX_MESSAGE_LENGTH

#define LPC_SECTION_MAX_SPACE_SIZE 0x1400000U /* 20 MiB */
#define LPC_PORT_NAME_LENGTH 30U

#define LPC_TYPE_REQUEST             1U
#define LPC_TYPE_REPLY               2U
#define LPC_TYPE_DATAGRAM            3U
#define LPC_TYPE_PORT_CLOSED         5U
#define LPC_TYPE_CONNECTION_REQUEST 10U
/* Kept as a source-compatible alias used by the old client implementation. */
#define LPC_TYPE_NEW_MESSAGE LPC_TYPE_REQUEST

/* These structures deliberately do not use PORT_MESSAGE from winternl.h:
 * that type is absent from some user-mode SDKs and differs in anonymous-union
 * spelling between SDK/WDK versions.  The binary layout is the documented
 * native layout and is passed through the function-pointer API below.  Users
 * normally do not construct these wire structures directly; LpcPort_SendMessage
 * and LpcPort_ProcessBlockedEventEx do it for them. */
#pragma pack(push, 8)
typedef struct _LPC_PORT_CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} LPC_PORT_CLIENT_ID, *PLPC_PORT_CLIENT_ID;

typedef struct _LPC_PORT_MESSAGE {
    union {
        struct {
            USHORT DataLength;
            USHORT TotalLength;
        } s1;
        ULONG Length;
    } u1;
    union {
        struct {
            USHORT Type;
            USHORT DataInfoOffset;
        } s2;
        ULONG ZeroInit;
    } u2;
    LPC_PORT_CLIENT_ID ClientId;
    ULONG MessageId;
    union {
        SIZE_T ClientViewSize;
        ULONG CallbackId;
    } u3;
} LPC_PORT_MESSAGE, *PLPC_PORT_MESSAGE;

typedef struct _LPC_PORT_VIEW {
    ULONG Length;
    HANDLE SectionHandle;
    ULONG SectionOffset;
    SIZE_T ViewSize;
    PVOID ViewBase;
    PVOID ViewRemoteBase;
} LPC_PORT_VIEW, *PLPC_PORT_VIEW;

typedef struct _LPC_REMOTE_PORT_VIEW {
    ULONG Length;
    SIZE_T ViewSize;
    PVOID ViewBase;
} LPC_REMOTE_PORT_VIEW, *PLPC_REMOTE_PORT_VIEW;
#pragma pack(pop)

#pragma pack(push, 8)
typedef struct _LPC_MESSAGE {
    ULONG size;
    UCHAR msg[LPC_MESSAGE_MAX_PACK_SIZE];
} LPC_MESSAGE, *PLPC_MESSAGE;

typedef struct _LPC_SHARED_MEMORY {
    ULONG size;
    UCHAR msg[ANYSIZE_ARRAY];
} LPC_SHARED_MEMORY, *PLPC_SHARED_MEMORY;

typedef struct _LPC_HEADER {
    LPC_PORT_MESSAGE header;
    ULONG ControlId;
    union {
        struct {
            ULONG ulUseSharedMemory : 1;
            ULONG ulUseAsyncMethod : 1;
            ULONG ulReserved : 30;
        } s1;
        ULONG options;
    } u1;
    UCHAR data[ANYSIZE_ARRAY];
} LPC_HEADER, *PLPC_HEADER;
#pragma pack(pop)

#define LPC_HEADER_DATA_OFFSET ((size_t)offsetof(LPC_HEADER, data))

/**
 * Receives a synchronous reply on the client thread that called
 * LpcPort_SendMessage.  `buffer` is valid only while the callback runs.
 * The callback's NTSTATUS return value is reserved and is not used to alter
 * the status returned by LpcPort_SendMessage.
 */
typedef NTSTATUS (NTAPI *typedef_LpcSyncMsgReplyCallback)(PVOID buffer, ULONG len, PVOID lpCtx);

/**
 * Invoked before a connection request is accepted.
 *
 * `lpRespCtrlId` initially contains the client's HelloId and may be changed
 * for the response.  `bDeny` is initialized to one (deny); set it to zero to
 * accept the connection.  The callback runs on the thread that calls a
 * LpcPort_ProcessBlockedEvent* function and must not retain either pointer.
 */
typedef void (*pfnLPCEvtOnPreConnect)(uint32_t *lpRespCtrlId, uint8_t *bDeny);

/** Invoked after a connection has been accepted and added to the client list.
 * The handle remains owned by the server until ServerDisconnectClient or
 * ServerClose; do not close it directly from the callback.  Do not call
 * ServerClose from any callback while an event-processing call is in flight.
 */
typedef void (*pfnLPCEvtOnPostConnect)(HANDLE hClientHandle, uint32_t ulCtrlId);

/**
 * Invoked for a synchronous request.  `msg` is the request payload and is
 * valid only for the duration of the callback.  Update it in place when the
 * same-sized payload is used as the reply; the SDK sends the LPC reply after
 * the callback returns.  Events for one client are serialized by its receive
 * lock.  Do not call back into the same server, ServerClose, or
 * ServerDisconnectClient while handling this event.
 */
typedef void (*pfnLPCEvtOnSyncRequest)(HANDLE hClientHandle, uint32_t ulCtrlId,
                                       uint8_t *msg, uint32_t size, uint64_t maxSize);

/**
 * Invoked for an asynchronous datagram.  The payload pointer is temporary and
 * no reply is sent.  Copy the data if it must outlive the callback.
 */
typedef void (*pfnLPCEvtOnAsyncRequest)(HANDLE hClientHandle, uint32_t ulCtrlId,
                                         uint8_t *msg, uint32_t size, uint64_t maxSize);

/** Invoked when the native port reports that a client endpoint was closed, or
 * when the endpoint is explicitly removed.  The client record has already
 * been detached, so a concurrent close cannot report it twice.  LPC has no
 * disconnect primitive; the SDK may close the native handle before invoking
 * this callback, so treat the handle as an identifier and do not use it.  Do
 * not call LpcPort_ServerClose or LpcPort_ServerDisconnectClient recursively
 * from this callback. */
typedef void (*pfnLPCEvtOnClose)(HANDLE hClientHandle);

/* Retained for source compatibility.  The current status-returning APIs do
 * not invoke this callback; inspect their NTSTATUS return value instead. */
typedef void (*pfnLPCEvtOnError)(uint32_t line, NTSTATUS status);

/**
 * Native LPC object name shared by the server and client.
 *
 * The array includes its terminating NUL byte.  Fill it directly (for
 * example, "\\RPC Control\\ExamplePort") and use exactly the same name on
 * both sides.  Naming policy stays in the application rather than in the
 * transport SDK.
 */
typedef struct _FC_LPC_PORTNAME {
    char name[LPC_PORT_NAME_LENGTH];
} LPC_PORT_NAME, *PLPC_PORT_NAME;

/* Historical spelling retained for source compatibility.  New code should
 * use LPC_PORT_NAME alongside the LpcPort_* function family. */
typedef LPC_PORT_NAME FC_LPC_PORTNAME;
typedef PLPC_PORT_NAME PFC_LPC_PORTNAME;

/**
 * Server event table.  Zero-initialize this structure and set only the events
 * the application needs before calling LpcPort_Register_ServerEvtCallback.
 * The table is copied, so the caller may release or reuse its instance after
 * registration.
 */
typedef struct _LPC_SERVER_EVT_CONTEXT {
    pfnLPCEvtOnPreConnect onPreConnect;
    pfnLPCEvtOnPostConnect onPostConnect;
    pfnLPCEvtOnSyncRequest onSyncRequest;
    pfnLPCEvtOnAsyncRequest onAsyncRequest;
    pfnLPCEvtOnClose onClose;
    pfnLPCEvtOnError onError;
} LPC_SERVER_EVT_CONTEXT, *PLPC_SERVER_EVT_CONTEXT;

/* Function pointers are supplied by the caller (usually an API resolver), so
 * neither user-mode imports nor kernel-mode syscall stubs are required here.
 * Resolve every function used by the selected side before calling Connect or
 * ServerCreate.  NTAPI is part of the function-pointer ABI on Windows. */
typedef void (NTAPI *typedef_RtlInitAnsiString)(PANSI_STRING DestinationString, PCSZ SourceString);
typedef NTSTATUS (NTAPI *typedef_NtCreateSection)(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                                                   POBJECT_ATTRIBUTES ObjectAttributes,
                                                   LARGE_INTEGER *MaximumSize, ULONG SectionPageProtection,
                                                   ULONG AllocationAttributes, HANDLE FileHandle);
typedef NTSTATUS (NTAPI *typedef_NtConnectPort)(PHANDLE PortHandle, PUNICODE_STRING PortName,
                                                 PSECURITY_QUALITY_OF_SERVICE SecurityQos,
                                                 PLPC_PORT_VIEW ClientView, PLPC_REMOTE_PORT_VIEW ServerView,
                                                 PULONG MaxMessageLength, PVOID ConnectionInformation,
                                                 PULONG ConnectionInformationLength);
typedef NTSTATUS (NTAPI *typedef_NtClose)(HANDLE Handle);
typedef NTSTATUS (NTAPI *typedef_RtlAnsiStringToUnicodeString)(PUNICODE_STRING DestinationString,
                                                                PANSI_STRING SourceString,
                                                                BOOLEAN AllocateDestinationString);
typedef void (NTAPI *typedef_RtlFreeUnicodeString)(PUNICODE_STRING UnicodeString);
typedef NTSTATUS (NTAPI *typedef_NtRequestPort)(HANDLE PortHandle, PLPC_PORT_MESSAGE RequestMessage);
typedef NTSTATUS (NTAPI *typedef_NtRequestWaitReplyPort)(HANDLE PortHandle,
                                                          PLPC_PORT_MESSAGE RequestMessage,
                                                          PLPC_PORT_MESSAGE ReplyMessage);

/** Client-side Native API table.  All members are required. */
typedef struct _LPC_CLIENT_APIS {
    typedef_RtlInitAnsiString pfnRtlInitAnsiString; /* Required string initializer. */
    typedef_NtCreateSection pfnNtCreateSection; /* Required for the client section. */
    typedef_NtConnectPort pfnNtConnectPort; /* Required connection call. */
    typedef_NtClose pfnNtClose; /* Required handle cleanup. */
    typedef_RtlAnsiStringToUnicodeString pfnRtlAnsiStringToUnicodeString;
    typedef_RtlFreeUnicodeString pfnRtlFreeUnicodeString;
    typedef_NtRequestPort pfnNtRequestPort; /* Required for async datagrams. */
    typedef_NtRequestWaitReplyPort pfnNtRequestWaitReplyPort; /* Required for sync sends. */
} LPC_CLIENT_APIS, *PLPC_CLIENT_APIS;

/* NtRequestWaitReplyPort has no timeout argument.  A client-side timeout
 * cannot be emulated safely because the pending LPC request cannot be
 * cancelled after the caller stops waiting. */

/**
 * Client creation settings.  Zero-initialize before filling the port name,
 * HelloId and resolved API table.  lpRespID is optional and receives the
 * server's response control ID on a successful connection.
 */
typedef struct _LPC_CLIENT_CONFIG {
    uint32_t HelloId;
    LPC_PORT_NAME LpcName;
    uint32_t *lpRespID;
    LPC_CLIENT_APIS api;
} LPC_CLIENT_CONFIG, *PLPC_CLIENT_CONFIG;

/**
 * Client-owned connection state.  Treat this structure as opaque after
 * LpcPort_Connect succeeds; use LpcPort_SendMessage and LpcPort_Disconnect
 * rather than changing handles or the section view directly.
 */
typedef struct _LPC_CLIENT_CONTEXT {
    HANDLE hLPCPortHandle;
    HANDLE hSectionHandle;
    UNICODE_STRING ustrLPCName;
    LPC_PORT_VIEW client_view;
    LPC_CLIENT_APIS api;
    /* Serializes section-backed transactions and protects disconnect. */
    LPC_PORT_LOCK sendLock;
    LPC_PORT_LIFETIME lifetime;
    /* Published with an interlocked store; zero means disconnected. */
    volatile LONG initialized;
} LPC_CLIENT_CONTEXT, *PLPC_CLIENT_CONTEXT;

typedef NTSTATUS (NTAPI *typedef_NtCreatePort)(PHANDLE PortHandle, POBJECT_ATTRIBUTES ObjectAttributes,
                                                ULONG MaxConnectionInfoLength, ULONG MaxMessageLength,
                                                ULONG MaxPoolUsage);
typedef NTSTATUS (NTAPI *typedef_NtReplyWaitReceivePort)(HANDLE PortHandle, PVOID *PortContext,
                                                          PVOID ReceivePortBuffer,
                                                          PLPC_PORT_MESSAGE ReceiveMessage);
typedef NTSTATUS (NTAPI *typedef_NtReplyWaitReceivePortEx)(HANDLE PortHandle,
                                                             PVOID *PortContext,
                                                             PVOID ReceivePortBuffer,
                                                             PLPC_PORT_MESSAGE ReceiveMessage,
                                                             PLARGE_INTEGER Timeout);
typedef NTSTATUS (NTAPI *typedef_NtAcceptConnectPort)(PHANDLE PortHandle, PVOID PortContext,
                                                        PLPC_PORT_MESSAGE ConnectionRequest,
                                                        BOOLEAN AcceptConnection, PLPC_PORT_VIEW ServerView,
                                                        PLPC_REMOTE_PORT_VIEW ClientView);
typedef NTSTATUS (NTAPI *typedef_NtCompleteConnectPort)(HANDLE PortHandle);
typedef NTSTATUS (NTAPI *typedef_NtReplyPort)(HANDLE PortHandle, PLPC_PORT_MESSAGE ReplyMessage);

/** Server-side Native API table.  The Ex receive routine is optional unless
 * the application uses LpcPort_ProcessBlockedEventEx with a timeout. */
typedef struct _LPC_SERVER_APIS {
    typedef_RtlInitAnsiString pfnRtlInitAnsiString; /* Required string initializer. */
    typedef_NtCreatePort pfnNtCreatePort; /* Required port creation call. */
    typedef_NtClose pfnNtClose; /* Required handle cleanup. */
    typedef_RtlAnsiStringToUnicodeString pfnRtlAnsiStringToUnicodeString;
    typedef_RtlFreeUnicodeString pfnRtlFreeUnicodeString;
    typedef_NtReplyWaitReceivePort pfnNtReplyWaitReceivePort; /* Infinite-wait receive. */
    typedef_NtAcceptConnectPort pfnNtAcceptConnectPort; /* Required connection accept. */
    typedef_NtCompleteConnectPort pfnNtCompleteConnectPort; /* Required connection completion. */
    typedef_NtReplyPort pfnNtReplyPort; /* Required synchronous reply. */
    /* Optional native extension. NULL disables timed receive. */
    typedef_NtReplyWaitReceivePortEx pfnNtReplyWaitReceivePortEx;
} LPC_SERVER_APIS, *PLPC_SERVER_APIS;

/**
 * Server creation settings.  Zero-initialize, fill LpcName and resolve the
 * required API table before passing this structure to LpcPort_ServerCreate.
 */
typedef struct _LPC_SERVER_CONFIG {
    LPC_PORT_NAME LpcName;
    /* NULL keeps the platform default DACL; pass an audited descriptor for
     * privileged services instead of relying on an implicit broad ACL.  The
     * descriptor is consumed during ServerCreate and need not remain alive. */
    PSECURITY_DESCRIPTOR securityDescriptor;
    /* Zero means no SDK limit.  A finite value is recommended for exposed
     * services to prevent unbounded client bookkeeping. */
    uint32_t maxClients;
    LPC_SERVER_APIS api;
} LPC_SERVER_CONFIG, *PLPC_SERVER_CONFIG;

/* Internal list entry owned by LPC_SERVER_CONTEXT; applications should not
 * create or modify this structure.  Its address is passed as Native
 * PortContext so the receive path can recover the communication handle.  It
 * is intentionally the first member because list.h accepts object pointers. */
typedef struct _LPC_SERVER_CLIENT_INFO {
    LIST_ELEM listEntry;
    HANDLE hLPCPortClientHandle;
    LPC_REMOTE_PORT_VIEW client_view;
    uint32_t controlId;
    /* Serializes receive/callback processing for this endpoint.  The lock is
     * non-recursive in kernel mode; callbacks must not process or disconnect
     * this same client recursively. */
    LPC_PORT_LOCK receiveLock;
    LPC_PORT_LIFETIME lifetime;
} LPC_SERVER_CLIENT_INFO, *PLPC_SERVER_CLIENT_INFO;

/**
 * Server-owned state.  Zero-initialize before LpcPort_ServerCreate and do not
 * copy it after initialization.  One event is processed per call to a
 * LpcPort_ProcessBlockedEvent* function.  The server may be closed only after
 * its event-processing calls have stopped.
 */
typedef struct _LPC_SERVER_CONTEXT {
    HANDLE hLPCPortServerHandle;
    UNICODE_STRING ustrLPCName;
    LPC_SERVER_EVT_CONTEXT ServerEvtCallback;
    /* lockWord protects only clientList/clientCount and callback snapshots.
     * Never hold it while invoking an on* callback or an LPC/native API. */
    LIST clientList;
    uint32_t clientCount;
    uint32_t maxClients;
    LPC_PORT_LOCK lockWord;
    LPC_PORT_LIFETIME lifetime;
    /* Published with an interlocked store; zero means closed. */
    volatile LONG initialized;
    LPC_SERVER_APIS api;
} LPC_SERVER_CONTEXT, *PLPC_SERVER_CONTEXT;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Typical setup:
 *
 *   LPC_SERVER_CONFIG serverConfig = {0};
 *   LPC_SERVER_CONTEXT serverContext = {0};
 *   serverConfig.LpcName = ...;       // same NUL-terminated name on both sides
 *   serverConfig.api = ...;           // resolve the Native API functions
 *   LpcPort_ServerCreate(&serverConfig, &serverContext);
 *   LpcPort_Register_ServerEvtCallback(&serverContext, &events);
 *   LpcPort_ProcessBlockedEventEx(&serverContext, &timeout);
 *   LpcPort_ServerClose(&serverContext);
 *
 * A client follows the analogous Connect -> SendMessage -> Disconnect order.
 * Keep each context in one owner/threading domain and stop the server event
 * loop before closing its context.  All functions are usable from C and C++;
 * the declarations use C linkage when included from C++.
 */

/**
 * Creates and publishes a named Native LPC server port.
 *
 * `lpServerCfg` and `lpLPCServerCtx` must be non-NULL and zero-initialized
 * before the first call.  The caller must provide all mandatory entries in
 * `lpServerCfg->api` and a valid NUL-terminated `LpcName`.  On success,
 * register callbacks and repeatedly call one of the ProcessBlockedEvent
 * functions.  Returns STATUS_DEVICE_BUSY for an already initialized context
 * and propagates Native API failures otherwise.
 */
NTSTATUS LpcPort_ServerCreate(PLPC_SERVER_CONFIG lpServerCfg, PLPC_SERVER_CONTEXT lpLPCServerCtx);

/**
 * Stops a server port, closes tracked client endpoints and releases its name.
 * Stop all threads calling LpcPort_ProcessBlockedEvent* before invoking this
 * function.  NULL and an already-closed context are safe no-op cases.  Do not
 * invoke it recursively from an event callback; signal an external shutdown
 * flag and let the owner thread perform the close.
 */
void LpcPort_ServerClose(PLPC_SERVER_CONTEXT lpLPCServerCtx);

/**
 * Creates a client section and connects it to the named server port.
 *
 * Zero-initialize both the config and context, fill the same `LpcName` used by
 * the server, and provide every entry in `lpClientCfg->api`.  On success the
 * context owns the port/section handles and must eventually be passed to
 * LpcPort_Disconnect (the historical LpcPort_DisConnect spelling is kept as
 * a compatibility wrapper).  `lpRespID` is optional and receives the negotiated
 * control ID.  Disconnect must be externally serialized against new sends; it
 * can wake an already in-flight send, but no caller may start another API call
 * after disconnect begins.
 */
NTSTATUS LpcPort_Connect(PLPC_CLIENT_CONFIG lpClientCfg, PLPC_CLIENT_CONTEXT lpLPCClientCtx);

/**
 * Sends one client message.
 *
 * Set `useAsyncMode` to a non-zero value for a fire-and-forget datagram; the
 * reply callback is ignored in that mode.  Synchronous sends use
 * NtRequestWaitReplyPort and optionally call `pfnSyncReplyCallback` after a
 * valid reply is received.  `useAsyncMode` and `useSharedMemory` cannot both
 * be non-zero.  Payloads larger than LPC_MESSAGE_MAX_PACK_SIZE are moved to
 * the negotiated section automatically for synchronous sends; large async
 * sends return STATUS_NOT_SUPPORTED.  The input buffer and callback context
 * are only accessed during this call.
 */
NTSTATUS LpcPort_SendMessage(PLPC_CLIENT_CONTEXT lpLPCClientCtx, const void *lpMsg,
                                    ULONG ulMsgLength, ULONG ControlId, uint8_t useAsyncMode,
                                    uint8_t useSharedMemory,
                                    typedef_LpcSyncMsgReplyCallback pfnSyncReplyCallback, void *lpCtx);

/** Closes a client port and section and releases the allocated port name. */
void LpcPort_DisConnect(PLPC_CLIENT_CONTEXT lpLPCClientCtx);

/** Preferred spelling of LpcPort_DisConnect; provided for new integrations. */
void LpcPort_Disconnect(PLPC_CLIENT_CONTEXT lpLPCClientCtx);

/**
 * Removes and closes one accepted client endpoint.  The handle is the value
 * delivered to server callbacks.  Call this after the corresponding callback
 * has returned; the SDK waits for in-flight work before freeing the endpoint.
 */
NTSTATUS LpcPort_ServerDisconnectClient(PLPC_SERVER_CONTEXT lpLPCServerCtx,
                                         HANDLE hClientHandle);

/**
 * Installs (copies) the server callback table.
 *
 * Call after LpcPort_ServerCreate and before entering the event loop.  The
 * callbacks are invoked without the internal list lock held, but they execute
 * synchronously on the event-processing thread; do not recursively process
 * the same server from a callback.
 */
uint8_t LpcPort_Register_ServerEvtCallback(PLPC_SERVER_CONTEXT lpLPCServerCtx,
                                                  const LPC_SERVER_EVT_CONTEXT *lpEvents);

/**
 * Waits indefinitely for and processes one server-side LPC event.
 * Equivalent to LpcPort_ProcessBlockedEventEx(lpLPCServerCtx, NULL).
 * Multiple event-loop threads are supported; events belonging to the same
 * accepted client are serialized by that client's receive lock, while
 * different clients may be processed concurrently.  Returns the Native
 * reply/receive status or a transport validation status.
 */
NTSTATUS LpcPort_ProcessBlockedEvent(PLPC_SERVER_CONTEXT lpLPCServerCtx);

/**
 * Waits for and processes one server-side LPC event with an optional timeout.
 *
 * A NULL timeout means an infinite wait.  Otherwise use the Native
 * LARGE_INTEGER convention: negative values are relative 100-ns intervals,
 * zero is an immediate poll, and positive values are absolute system time.
 * `STATUS_TIMEOUT` means no event arrived before the deadline.  A non-NULL
 * timeout requires pfnNtReplyWaitReceivePortEx in the server API table.
 */
NTSTATUS LpcPort_ProcessBlockedEventEx(PLPC_SERVER_CONTEXT lpLPCServerCtx,
                                             const LARGE_INTEGER *lpTimeout);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* Keep existing source clients buildable while making the LpcPort_ names the
 * canonical public interface.  Define LPC_PORT_NO_LEGACY_ALIASES when an
 * integrator wants to detect and remove old names during migration. */
#ifndef LPC_PORT_NO_LEGACY_ALIASES
#  define Interface_LPC_ServerCreate LpcPort_ServerCreate
#  define Interface_LPC_ServerClose LpcPort_ServerClose
#  define Interface_LPC_Connect LpcPort_Connect
#  define Interface_LPC_SendMessage LpcPort_SendMessage
#  define Interface_LPC_DisConnect LpcPort_DisConnect
#  define Interface_LPC_Disconnect LpcPort_Disconnect
#  define Interface_LPC_ServerDisconnectClient LpcPort_ServerDisconnectClient
#  define Interface_LPC_Register_ServerEvtCallback LpcPort_Register_ServerEvtCallback
#  define Interface_LPC_ProcessBlockedEvent LpcPort_ProcessBlockedEvent
#  define Interface_LPC_ProcessBlockedEventEx LpcPort_ProcessBlockedEventEx
#endif

#endif /* LPC_PORT_H_INCLUDED */
