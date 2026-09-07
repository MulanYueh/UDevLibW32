/*
 * Native ALPC transport shared by Windows user-mode and kernel-mode code.
 *
 * The SDK intentionally uses resolved Native API function pointers instead of
 * importing ntdll/ntoskrnl symbols.  This keeps one source compatible with
 * user-mode, kernel-mode and C++ callers.  ALPC has a connection port and a
 * separate communication port for every accepted client; the server API
 * exposes both phases explicitly so applications can assign one worker to
 * each communication port without hidden threads.
 */
#ifndef ALPC_PORT_H_INCLUDED
#define ALPC_PORT_H_INCLUDED

#if !defined(OXDRV_KLIBC_H_INCLUDED) && !defined(_STDINT_H) && \
    !defined(_STDINT_H_) && !defined(_GCC_WRAP_STDINT_H)
#  if defined(ALPC_PORT_USE_STDINT2) || \
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

#if defined(_KERNEL_MODE)
#  if defined(_MSC_VER)
#    include <ntifs.h>
#    define ALPC_PORT_HAS_PLATFORM_HEADERS 1
#  elif defined(__has_include)
#    if __has_include(<ntifs.h>)
#      include <ntifs.h>
#      define ALPC_PORT_HAS_PLATFORM_HEADERS 1
#    endif
#  endif
#else
#  if defined(_MSC_VER)
#    include <windows.h>
#    include <winternl.h>
#    define ALPC_PORT_HAS_PLATFORM_HEADERS 1
#  elif defined(__has_include)
#    if __has_include(<windows.h>)
#      include <windows.h>
#      include <winternl.h>
#      define ALPC_PORT_HAS_PLATFORM_HEADERS 1
#    endif
#  endif
#endif

#ifndef ALPC_PORT_HAS_PLATFORM_HEADERS
#  define ALPC_PORT_HAS_PLATFORM_HEADERS 0
#  ifndef LPC_ALPC_PORT_COMMON_TYPES_DEFINED
#    define LPC_ALPC_PORT_COMMON_TYPES_DEFINED 1
typedef uint8_t UCHAR;
typedef uint8_t BOOLEAN;
typedef uint16_t USHORT;
/* Keep the freestanding spelling identical to allocator.h's fallback; this
 * avoids a C++ typedef conflict when either header is included first. */
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
typedef struct _ALPC_LARGE_INTEGER { LONGLONG QuadPart; } LARGE_INTEGER;
typedef LARGE_INTEGER *PLARGE_INTEGER;
typedef struct _ALPC_ANSI_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
} ANSI_STRING, *PANSI_STRING;
typedef struct _ALPC_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    wchar_t *Buffer;
} UNICODE_STRING, *PUNICODE_STRING;
typedef const UNICODE_STRING *PCUNICODE_STRING;
typedef struct _ALPC_OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;
typedef struct _ALPC_SECURITY_QUALITY_OF_SERVICE {
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
#  ifndef SecurityImpersonation
#    define SecurityImpersonation 2UL
#  endif
#  ifndef SECURITY_DYNAMIC_TRACKING
#    define SECURITY_DYNAMIC_TRACKING 1
#  endif
#endif

#include "list.h"

/* ALPC serializes the endpoint state internally.  This lock protects only
 * the SDK's client list and callback snapshot; no native API or callback is
 * called while it is held.  PushLock is therefore deliberately non-recursive
 * in kernel mode, while the user-mode CriticalSection is recursive by design. */
#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
typedef struct _ALPC_PORT_LOCK {
    EX_PUSH_LOCK native;
    uint8_t initialized;
} ALPC_PORT_LOCK;
#elif !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
typedef struct _ALPC_PORT_LOCK {
    CRITICAL_SECTION native;
    uint8_t initialized;
} ALPC_PORT_LOCK;
#else
typedef struct _ALPC_PORT_LOCK {
    volatile uint32_t native;
    uint8_t initialized;
} ALPC_PORT_LOCK;
#endif

/* Protects object lifetime, not just the client list.  A successful acquire
 * pins the object until the matching release; shutdown blocks new acquires and
 * waits for existing users before closing any Native handle. */
#if defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
typedef struct _ALPC_PORT_LIFETIME {
    EX_RUNDOWN_REF rundown;
    volatile LONG admission;
    volatile LONG closing;
    uint8_t initialized;
} ALPC_PORT_LIFETIME;
#elif !defined(_KERNEL_MODE) && ALPC_PORT_HAS_PLATFORM_HEADERS
typedef struct _ALPC_PORT_LIFETIME {
    CRITICAL_SECTION native;
    CONDITION_VARIABLE idle;
    uint32_t active;
    uint8_t closing;
    uint8_t initialized;
} ALPC_PORT_LIFETIME;
#else
typedef struct _ALPC_PORT_LIFETIME {
    volatile uint32_t native;
    volatile uint32_t active;
    uint8_t closing;
    uint8_t initialized;
} ALPC_PORT_LIFETIME;
#endif

#if !defined(ALPC_PORT_NO_LIBC_HEADER) && !defined(OXDRV_KLIBC_H_INCLUDED)
#  ifndef _STDINT_H
#    define _STDINT_H
#  endif
#  if defined(_MSC_VER) && !defined(_STDINT_H_)
#    define _STDINT_H_
#  endif
#  include "libc.h"
#endif

/* ALPC accepts larger messages than legacy LPC.  The limit is intentionally
 * bounded so a malformed peer cannot make the SDK allocate unbounded memory. */
#define ALPC_PORT_DEFAULT_MAX_MESSAGE_LENGTH ((SIZE_T)0x1000U)
#define ALPC_PORT_HARD_MAX_MESSAGE_LENGTH    ((SIZE_T)0xFF00U)
#define ALPC_PORT_NAME_LENGTH                256U

#define ALPC_PORT_MESSAGE_TYPE_REQUEST            0x2001U
#define ALPC_PORT_MESSAGE_TYPE_REPLY              0x2002U
#define ALPC_PORT_MESSAGE_TYPE_DATAGRAM           0x2003U
#define ALPC_PORT_MESSAGE_TYPE_PORT_CLOSED        0x2005U
#define ALPC_PORT_MESSAGE_TYPE_CONNECTION_REQUEST 0x200AU
#define ALPC_PORT_SEND_FLAG_REPLY_MESSAGE         0x00000001UL
#define ALPC_PORT_SEND_FLAG_ASYNC                 0x00000001UL

/* Common values accepted in the portFlags fields.  Keep flags at zero unless
 * the deployment explicitly needs impersonation or duplicate-object support. */
#define ALPC_PORT_FLAG_NONE                       0x00000000UL
#define ALPC_PORT_FLAG_ALLOW_IMPERSONATION       0x00010000UL
#define ALPC_PORT_FLAG_ALLOW_LPC_REQUESTS        0x00020000UL
#define ALPC_PORT_FLAG_WAITABLE_PORT              0x00040000UL
#define ALPC_PORT_FLAG_ALLOW_DUP_OBJECT           0x00080000UL

#pragma pack(push, 8)
/** Native PORT_MESSAGE-compatible header used by ALPC system calls. */
typedef struct _ALPC_PORT_CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} ALPC_PORT_CLIENT_ID, *PALPC_PORT_CLIENT_ID;

typedef struct _ALPC_PORT_MESSAGE {
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
    ALPC_PORT_CLIENT_ID ClientId;
    ULONG MessageId;
    union {
        SIZE_T ClientViewSize;
        ULONG CallbackId;
    } u3;
} ALPC_PORT_MESSAGE, *PALPC_PORT_MESSAGE;

/** SDK frame carried as the payload of an ALPC PORT_MESSAGE. */
typedef struct _ALPC_PORT_FRAME {
    ALPC_PORT_MESSAGE header;
    ULONG ControlId;
    ULONG Flags;
    ULONG PayloadLength;
    /* Server processing status.  Requests must carry STATUS_SUCCESS; replies
     * carry the callback result so transport success and business failure are
     * distinguishable by the client. */
    NTSTATUS Status;
    UCHAR data[ANYSIZE_ARRAY];
} ALPC_PORT_FRAME, *PALPC_PORT_FRAME;

/** Native ALPC port limits passed to NtAlpcCreatePort/ConnectPort. */
typedef struct _ALPC_PORT_SDK_ATTRIBUTES {
    ULONG Flags;
    SECURITY_QUALITY_OF_SERVICE SecurityQos;
    SIZE_T MaxMessageLength;
    SIZE_T MemoryBandwidth;
    SIZE_T MaxPoolUsage;
    SIZE_T MaxSectionSize;
    SIZE_T MaxViewSize;
    SIZE_T MaxTotalSectionSize;
    ULONG DupObjectTypes;
#if defined(_WIN64) || defined(_M_AMD64) || defined(__x86_64__) || \
    defined(__amd64__) || defined(_M_ARM64) || defined(__aarch64__)
    ULONG Reserved;
#endif
} ALPC_PORT_SDK_ATTRIBUTES, *PALPC_PORT_SDK_ATTRIBUTES;
#pragma pack(pop)

#define ALPC_PORT_FRAME_DATA_OFFSET ((size_t)offsetof(ALPC_PORT_FRAME, data))

/** Null-terminated Native object name shared by client and server. */
typedef struct _ALPC_PORT_NAME {
    char name[ALPC_PORT_NAME_LENGTH];
} ALPC_PORT_NAME, *PALPC_PORT_NAME;

/** Receives a synchronous client reply; the buffer is temporary. */
typedef NTSTATUS (NTAPI *AlpcPort_SyncReplyCallback)(const uint8_t *buffer,
                                                      ULONG length,
                                                      PVOID callbackContext);

/** Runs before the server accepts a connection; deny is initialized to one. */
typedef void (*AlpcPort_PreConnectCallback)(uint32_t *responseControlId,
                                             uint8_t *deny,
                                             PVOID callbackContext);

/** Runs after a communication port has been accepted and completed.  The
 * SDK owns clientPort and closes it after an explicit disconnect or shutdown;
 * workers should only use the handle with ProcessClientEvent*.  Do not call
 * AlpcPort_ServerClose from any callback while an event-processing call is in
 * flight. */
typedef void (*AlpcPort_PostConnectCallback)(HANDLE clientPort,
                                              const ALPC_PORT_CLIENT_ID *clientId,
                                              uint32_t controlId,
                                              PVOID callbackContext);

/** Handles a synchronous request and may change payloadLength for the reply.
 * The callback runs while that client's non-recursive receive lock is held;
 * do not call AlpcPort_ProcessClientEvent*, AlpcPort_ServerDisconnectClient,
 * or AlpcPort_ServerClose from this callback. */
typedef NTSTATUS (*AlpcPort_SyncRequestCallback)(HANDLE clientPort,
                                                  const ALPC_PORT_CLIENT_ID *clientId,
                                                  uint32_t controlId,
                                                  uint8_t *payload,
                                                  ULONG *payloadLength,
                                                  ULONG maxPayloadLength,
                                                  PVOID callbackContext);

/** Handles an asynchronous datagram; no reply is generated.  The same
 * non-recursive receive-lock restriction as the synchronous callback applies
 * while this routine executes; do not close the server recursively. */
typedef NTSTATUS (*AlpcPort_AsyncRequestCallback)(HANDLE clientPort,
                                                   const ALPC_PORT_CLIENT_ID *clientId,
                                                   uint32_t controlId,
                                                   const uint8_t *payload,
                                                   ULONG payloadLength,
                                                   PVOID callbackContext);

/** Runs when a client endpoint is disconnected or explicitly removed.  The
 * handle is still valid during the callback and is closed immediately after.
 * Do not call AlpcPort_ServerClose or AlpcPort_ServerDisconnectClient
 * recursively from this callback. */
typedef void (*AlpcPort_CloseCallback)(HANDLE clientPort,
                                        const ALPC_PORT_CLIENT_ID *clientId,
                                        PVOID callbackContext);

/**
 * Server callback table.  Zero-initialize it and fill only the events needed
 * by the application.  The table is copied by AlpcPort_Register_ServerEvtCallback.
 */
typedef struct _ALPC_PORT_SERVER_EVENTS {
    AlpcPort_PreConnectCallback onPreConnect;
    AlpcPort_PostConnectCallback onPostConnect;
    AlpcPort_SyncRequestCallback onSyncRequest;
    AlpcPort_AsyncRequestCallback onAsyncRequest;
    AlpcPort_CloseCallback onClose;
    PVOID callbackContext;
} ALPC_PORT_SERVER_EVENTS, *PALPC_PORT_SERVER_EVENTS;

/* Native API resolver types.  The NTAPI calling convention is required on
 * Windows; callers may populate these fields from ntdll/ntoskrnl exports. */
typedef void (NTAPI *AlpcPort_RtlInitAnsiString)(PANSI_STRING destination,
                                                  PCSZ source);
typedef NTSTATUS (NTAPI *AlpcPort_RtlAnsiStringToUnicodeString)(PUNICODE_STRING destination,
                                                                  PANSI_STRING source,
                                                                  BOOLEAN allocateDestination);
typedef void (NTAPI *AlpcPort_RtlFreeUnicodeString)(PUNICODE_STRING string);
typedef NTSTATUS (NTAPI *AlpcPort_NtClose)(HANDLE handle);
typedef NTSTATUS (NTAPI *AlpcPort_NtAlpcCreatePort)(PHANDLE portHandle,
                                                     POBJECT_ATTRIBUTES objectAttributes,
                                                     PALPC_PORT_SDK_ATTRIBUTES portAttributes);
typedef NTSTATUS (NTAPI *AlpcPort_NtAlpcConnectPort)(PHANDLE portHandle,
                                                      PCUNICODE_STRING portName,
                                                      POBJECT_ATTRIBUTES objectAttributes,
                                                      PALPC_PORT_SDK_ATTRIBUTES portAttributes,
                                                      ULONG flags,
                                                      PSECURITY_DESCRIPTOR requiredServerSid,
                                                      PALPC_PORT_MESSAGE connectionMessage,
                                                      PSIZE_T bufferLength,
                                                      PVOID outMessageAttributes,
                                                      PVOID inMessageAttributes,
                                                      PLARGE_INTEGER timeout);
typedef NTSTATUS (NTAPI *AlpcPort_NtAlpcAcceptConnectPort)(PHANDLE portHandle,
                                                            HANDLE connectionPortHandle,
                                                            ULONG flags,
                                                            POBJECT_ATTRIBUTES objectAttributes,
                                                            PALPC_PORT_SDK_ATTRIBUTES portAttributes,
                                                            PVOID portContext,
                                                            PALPC_PORT_MESSAGE connectionRequest,
                                                            PVOID connectionMessageAttributes,
                                                            BOOLEAN acceptConnection);
typedef NTSTATUS (NTAPI *AlpcPort_NtAlpcCompleteConnectPort)(HANDLE portHandle);
typedef NTSTATUS (NTAPI *AlpcPort_NtAlpcSendWaitReceivePort)(HANDLE portHandle,
                                                              ULONG flags,
                                                              PALPC_PORT_MESSAGE sendMessage,
                                                              PVOID sendMessageAttributes,
                                                              PALPC_PORT_MESSAGE receiveMessage,
                                                              PSIZE_T bufferLength,
                                                              PVOID receiveMessageAttributes,
                                                              PLARGE_INTEGER timeout);
typedef NTSTATUS (NTAPI *AlpcPort_NtAlpcDisconnectPort)(HANDLE portHandle, ULONG flags);

/** Client-side resolver table.  Every field is mandatory. */
typedef struct _ALPC_PORT_CLIENT_APIS {
    AlpcPort_RtlInitAnsiString pfnRtlInitAnsiString;
    AlpcPort_RtlAnsiStringToUnicodeString pfnRtlAnsiStringToUnicodeString;
    AlpcPort_RtlFreeUnicodeString pfnRtlFreeUnicodeString;
    AlpcPort_NtClose pfnNtClose;
    AlpcPort_NtAlpcConnectPort pfnNtAlpcConnectPort;
    AlpcPort_NtAlpcSendWaitReceivePort pfnNtAlpcSendWaitReceivePort;
    AlpcPort_NtAlpcDisconnectPort pfnNtAlpcDisconnectPort;
} ALPC_PORT_CLIENT_APIS, *PALPC_PORT_CLIENT_APIS;

/** Server-side resolver table.  Every field is mandatory. */
typedef struct _ALPC_PORT_SERVER_APIS {
    AlpcPort_RtlInitAnsiString pfnRtlInitAnsiString;
    AlpcPort_RtlAnsiStringToUnicodeString pfnRtlAnsiStringToUnicodeString;
    AlpcPort_RtlFreeUnicodeString pfnRtlFreeUnicodeString;
    AlpcPort_NtClose pfnNtClose;
    AlpcPort_NtAlpcCreatePort pfnNtAlpcCreatePort;
    AlpcPort_NtAlpcAcceptConnectPort pfnNtAlpcAcceptConnectPort;
    AlpcPort_NtAlpcCompleteConnectPort pfnNtAlpcCompleteConnectPort;
    AlpcPort_NtAlpcSendWaitReceivePort pfnNtAlpcSendWaitReceivePort;
    AlpcPort_NtAlpcDisconnectPort pfnNtAlpcDisconnectPort;
} ALPC_PORT_SERVER_APIS, *PALPC_PORT_SERVER_APIS;

/** Client creation settings; zero-initialize before use. */
typedef struct _ALPC_PORT_CLIENT_CONFIG {
    ALPC_PORT_NAME portName;
    uint32_t helloId;
    ULONG portFlags;
    SIZE_T maxMessageLength; /* Total native frame size; zero selects the default. */
    SECURITY_QUALITY_OF_SERVICE securityQos;
    ALPC_PORT_CLIENT_APIS api;
    const LARGE_INTEGER *connectTimeout;
} ALPC_PORT_CLIENT_CONFIG, *PALPC_PORT_CLIENT_CONFIG;

/** Client-owned state.  Do not modify fields after AlpcPort_Connect succeeds. */
typedef struct _ALPC_PORT_CLIENT_CONTEXT {
    HANDLE portHandle;
    UNICODE_STRING unicodeName;
    SIZE_T maxMessageLength;
    uint32_t negotiatedControlId;
    ALPC_PORT_LOCK sendLock;
    ALPC_PORT_LIFETIME lifetime;
    /* Published with an interlocked store; zero means disconnected. */
    volatile LONG initialized;
    ALPC_PORT_CLIENT_APIS api;
} ALPC_PORT_CLIENT_CONTEXT, *PALPC_PORT_CLIENT_CONTEXT;

/** Server creation settings; zero-initialize before use. */
typedef struct _ALPC_PORT_SERVER_CONFIG {
    ALPC_PORT_NAME portName;
    ULONG portFlags;
    SIZE_T maxMessageLength; /* Must match clients; zero selects the default. */
    SIZE_T maxPoolUsage;
    SIZE_T maxSectionSize;
    SIZE_T maxViewSize;
    SIZE_T maxTotalSectionSize;
    /* Zero means no SDK limit.  A finite value is recommended for exposed
     * services to bound endpoint memory and worker count. */
    uint32_t maxClients;
    SECURITY_QUALITY_OF_SERVICE securityQos;
    PSECURITY_DESCRIPTOR securityDescriptor;
    ALPC_PORT_SERVER_APIS api;
} ALPC_PORT_SERVER_CONFIG, *PALPC_PORT_SERVER_CONFIG;

typedef struct _ALPC_PORT_SERVER_CLIENT {
    /* Must remain first: list.h operates on object pointers directly. */
    LIST_ELEM listEntry;
    HANDLE portHandle;
    ALPC_PORT_CLIENT_ID clientId;
    uint32_t controlId;
    ALPC_PORT_LOCK receiveLock;
    ALPC_PORT_LIFETIME lifetime;
} ALPC_PORT_SERVER_CLIENT, *PALPC_PORT_SERVER_CLIENT;

/** Server-owned state; one context may service many communication ports. */
typedef struct _ALPC_PORT_SERVER_CONTEXT {
    HANDLE connectionPortHandle;
    UNICODE_STRING unicodeName;
    SIZE_T maxMessageLength;
    ALPC_PORT_SERVER_EVENTS events;
    LIST clientList;
    uint32_t clientCount;
    uint32_t maxClients;
    ALPC_PORT_LOCK lockWord;
    ALPC_PORT_LIFETIME lifetime;
    /* Published with an interlocked store; zero means closed. */
    volatile LONG initialized;
    ALPC_PORT_SERVER_APIS api;
} ALPC_PORT_SERVER_CONTEXT, *PALPC_PORT_SERVER_CONTEXT;

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal integration outline (the Native API resolver is application-owned):
 *
 *   ALPC_PORT_SERVER_CONFIG sc = {0};
 *   ALPC_PORT_SERVER_CONTEXT sx = {0};
 *   sc.portName = ...; sc.api = ...;
 *   AlpcPort_ServerCreate(&sc, &sx);
 *   AlpcPort_Register_ServerEvtCallback(&sx, &events);
 *   for (;;) AlpcPort_ProcessBlockedEventEx(&sx, &relativeTimeout);
 *
 * The onPostConnect callback receives each communication-port handle.  Hand
 * that handle to a worker and have the worker call
 * AlpcPort_ProcessClientEventEx(&sx, clientPort, &relativeTimeout).  A client
 * uses the analogous Connect -> SendMessage -> Disconnect sequence. */

/**
 * Creates the named ALPC connection port.  Fill the resolved API table and
 * use the same NUL-terminated portName on clients.  The context must be zero
 * initialized and must outlive all event-processing calls.
 */
NTSTATUS AlpcPort_ServerCreate(PALPC_PORT_SERVER_CONFIG config,
                               PALPC_PORT_SERVER_CONTEXT context);

/**
 * Stops the connection port and disconnects/frees all tracked clients.  New
 * operations are rejected and the call waits for in-flight workers.  Do not
 * invoke it recursively from an event callback; stop external wait loops
 * first when using an infinite timeout.  A callback may request shutdown by
 * setting an external flag and letting its owner thread call ServerClose.
 */
void AlpcPort_ServerClose(PALPC_PORT_SERVER_CONTEXT context);

/** Installs a copied callback table; call after ServerCreate and before wait. */
uint8_t AlpcPort_Register_ServerEvtCallback(PALPC_PORT_SERVER_CONTEXT context,
                                             const ALPC_PORT_SERVER_EVENTS *events);

/**
 * Waits indefinitely for one connection request on the server connection
 * port.  Accepted clients are reported through onPostConnect and must be
 * serviced with AlpcPort_ProcessClientEvent* (usually by a worker thread).
 */
NTSTATUS AlpcPort_ProcessBlockedEvent(PALPC_PORT_SERVER_CONTEXT context);

/**
 * Processes one connection request with an optional Native timeout.  A NULL
 * timeout waits forever; negative LARGE_INTEGER values are relative 100-ns
 * intervals, zero polls immediately, and positive values are absolute time.
 */
NTSTATUS AlpcPort_ProcessBlockedEventEx(PALPC_PORT_SERVER_CONTEXT context,
                                         const LARGE_INTEGER *timeout);

/**
 * Waits indefinitely for one message on a specific accepted client port.
 * The SDK serializes workers per clientPort; separate client ports may be
 * processed concurrently.  The callback's NTSTATUS is sent in the reply and
 * is returned by the client-side SendMessage call.
 */
NTSTATUS AlpcPort_ProcessClientEvent(PALPC_PORT_SERVER_CONTEXT context,
                                      HANDLE clientPort);

/** Processes one message on a client port with the same optional timeout form. */
NTSTATUS AlpcPort_ProcessClientEventEx(PALPC_PORT_SERVER_CONTEXT context,
                                        HANDLE clientPort,
                                        const LARGE_INTEGER *timeout);

/**
 * Explicitly disconnects and removes one accepted client communication port.
 * The SDK waits for that client's active operation before closing its handle.
 * Do not call it from that same client's callback.
 */
NTSTATUS AlpcPort_ServerDisconnectClient(PALPC_PORT_SERVER_CONTEXT context,
                                           HANDLE clientPort);

/** Connects a client, optionally waiting only until connectTimeout expires. */
NTSTATUS AlpcPort_Connect(PALPC_PORT_CLIENT_CONFIG config,
                          PALPC_PORT_CLIENT_CONTEXT context);

/**
 * Sends a request or datagram.  Set ALPC_PORT_SEND_FLAG_ASYNC for a datagram;
 * otherwise the call waits for a reply.  The timeout applies to the native
 * send/receive wait, and the reply callback is optional.  Payloads must fit
 * within the configured maxMessageLength; larger payloads return
 * STATUS_INFO_LENGTH_MISMATCH instead of allocating unbounded memory.  A
 * synchronous server callback status is returned to the caller (and the
 * reply callback still receives the payload), so transport success and
 * application failure remain distinguishable.  A
 * timed-out request may already have reached the server, so operations should
 * be idempotent when retrying.
 */
NTSTATUS AlpcPort_SendMessage(PALPC_PORT_CLIENT_CONTEXT context,
                              const void *message,
                              ULONG messageLength,
                              ULONG controlId,
                              ULONG flags,
                              AlpcPort_SyncReplyCallback replyCallback,
                              PVOID callbackContext,
                              const LARGE_INTEGER *timeout);

/** Closes the client communication port and releases its converted name.
 * Serialize Disconnect against new SendMessage calls.  A call already inside
 * SendMessage is drained by the lifetime gate after the native disconnect
 * wakes it; callers must not start another operation once shutdown begins. */
void AlpcPort_DisConnect(PALPC_PORT_CLIENT_CONTEXT context);

/** Preferred spelling of AlpcPort_DisConnect; provided for new integrations. */
void AlpcPort_Disconnect(PALPC_PORT_CLIENT_CONTEXT context);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALPC_PORT_H_INCLUDED */
