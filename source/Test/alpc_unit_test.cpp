/*
 * Catch2 regression tests for the user-mode ALPC SDK.
 *
 * A deterministic in-process Native API double exercises the public ALPC
 * state machine without requiring a Windows kernel or a second process.  Build
 * alpc_port.c as C and link this file as C++:
 *
 *   gcc -std=c11 -I. -c alpc_port.c allocator.c libc.c list.c
 *   g++ -std=c++17 -I. alpc_unit_test.cpp alpc_port.o allocator.o libc.o list.o \
 *       -pthread -o alpc_unit_test.exe
 */

#define CATCH_CONFIG_MAIN
#include "catch2/catch.hpp"

#include "alpc_port.h"

/* Normalize MinGW's optional unsigned ntstatus macros for strict C++ builds. */
#undef STATUS_SUCCESS
#undef STATUS_UNSUCCESSFUL
#undef STATUS_INVALID_PARAMETER
#undef STATUS_INFO_LENGTH_MISMATCH
#undef STATUS_DATA_ERROR
#undef STATUS_INSUFFICIENT_RESOURCES
#undef STATUS_INTEGER_OVERFLOW
#undef STATUS_NOT_SUPPORTED
#undef STATUS_DEVICE_BUSY
#undef STATUS_TIMEOUT
#undef STATUS_PORT_DISCONNECTED
#define STATUS_SUCCESS static_cast<NTSTATUS>(static_cast<int32_t>(0x00000000U))
#define STATUS_UNSUCCESSFUL static_cast<NTSTATUS>(static_cast<int32_t>(0xC0000001U))
#define STATUS_INVALID_PARAMETER static_cast<NTSTATUS>(static_cast<int32_t>(0xC000000DU))
#define STATUS_INFO_LENGTH_MISMATCH static_cast<NTSTATUS>(static_cast<int32_t>(0xC0000004U))
#define STATUS_DATA_ERROR static_cast<NTSTATUS>(static_cast<int32_t>(0xC000003EU))
#define STATUS_INSUFFICIENT_RESOURCES static_cast<NTSTATUS>(static_cast<int32_t>(0xC000009AU))
#define STATUS_INTEGER_OVERFLOW static_cast<NTSTATUS>(static_cast<int32_t>(0xC0000095U))
#define STATUS_NOT_SUPPORTED static_cast<NTSTATUS>(static_cast<int32_t>(0xC00000BBU))
#define STATUS_DEVICE_BUSY static_cast<NTSTATUS>(static_cast<int32_t>(0xC00000E8U))
#define STATUS_TIMEOUT static_cast<NTSTATUS>(static_cast<int32_t>(0x00000102U))
#define STATUS_PORT_DISCONNECTED static_cast<NTSTATUS>(static_cast<int32_t>(0xC0000037U))

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace
{

static constexpr SIZE_T kAlpcCapacity = static_cast<SIZE_T>(0x1000U);
static constexpr USHORT kAlpcConnectionCompleteType =
    static_cast<USHORT>(0x000BU);

struct FakeAlpcServer
{
    bool closed;

    FakeAlpcServer() : closed(false)
    {
    }
};

struct FakeAlpcSync;

struct FakeAlpcEndpoint
{
    PVOID serverContext;
    FakeAlpcSync *pendingSync;
    bool closed;

    FakeAlpcEndpoint()
        : serverContext(static_cast<PVOID>(0)),
          pendingSync(static_cast<FakeAlpcSync *>(0)), closed(false)
    {
    }
};

struct FakeAlpcConnect
{
    FakeAlpcEndpoint *endpoint;
    uint32_t responseControlId;
    bool accepted;
    bool denied;

    FakeAlpcConnect()
        : endpoint(static_cast<FakeAlpcEndpoint *>(0)), responseControlId(0U),
          accepted(false), denied(false)
    {
    }
};

struct FakeAlpcSync
{
    std::mutex mutex;
    std::condition_variable condition;
    std::array<unsigned char, kAlpcCapacity> reply;
    SIZE_T replyLength;
    NTSTATUS status;
    bool replied;

    FakeAlpcSync()
        : mutex(), condition(), reply(), replyLength(0U),
          status(STATUS_PORT_DISCONNECTED), replied(false)
    {
    }
};

struct FakeAlpcQueued
{
    std::array<unsigned char, kAlpcCapacity> bytes;
    SIZE_T length;
    FakeAlpcEndpoint *endpoint;
    FakeAlpcConnect *connect;
    FakeAlpcSync *sync;

    FakeAlpcQueued()
        : bytes(), length(0U), endpoint(static_cast<FakeAlpcEndpoint *>(0)),
          connect(static_cast<FakeAlpcConnect *>(0)),
          sync(static_cast<FakeAlpcSync *>(0))
    {
    }
};

struct FakeAlpcState
{
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<FakeAlpcQueued> queue;
    FakeAlpcServer *server;
    ALPC_PORT_SERVER_CONTEXT *serverContext;
    FakeAlpcQueued current;
    bool hasCurrent;
    bool mutateNextRequest;
    bool appendTrailingByteNextRequest;
    PVOID portContextOverride;
    NTSTATUS emptyEndpointPollStatus;
    uint32_t replySendOnlyCalls;
    uint32_t replyWithReceiveCalls;
    uint32_t lastReplyControlId;
    NTSTATUS nextReplyStatus;
    uint32_t connectionCompleteDeliveries;
    LONGLONG connectElapsedTicks;
    LONGLONG lastConnectTimeout;
    LONGLONG lastEndpointReceiveTimeout;
    USHORT nativeTypeHighBits;
    std::vector<FakeAlpcServer *> servers;
    std::vector<FakeAlpcEndpoint *> endpoints;

    FakeAlpcState()
        : mutex(), condition(), queue(), server(static_cast<FakeAlpcServer *>(0)),
          serverContext(static_cast<ALPC_PORT_SERVER_CONTEXT *>(0)), current(),
          hasCurrent(false), mutateNextRequest(false),
          appendTrailingByteNextRequest(false),
          portContextOverride(static_cast<PVOID>(0)),
          emptyEndpointPollStatus(STATUS_TIMEOUT), replySendOnlyCalls(0U),
          replyWithReceiveCalls(0U), lastReplyControlId(0U),
          nextReplyStatus(STATUS_SUCCESS), connectionCompleteDeliveries(0U),
          connectElapsedTicks(0), lastConnectTimeout(0),
          lastEndpointReceiveTimeout(0), nativeTypeHighBits(0U), servers(),
          endpoints()
    {
    }

    void reset()
    {
        std::lock_guard<std::mutex> guard(mutex);
        queue.clear();
        server = static_cast<FakeAlpcServer *>(0);
        serverContext = static_cast<ALPC_PORT_SERVER_CONTEXT *>(0);
        current = FakeAlpcQueued();
        hasCurrent = false;
        mutateNextRequest = false;
        appendTrailingByteNextRequest = false;
        portContextOverride = static_cast<PVOID>(0);
        emptyEndpointPollStatus = STATUS_TIMEOUT;
        replySendOnlyCalls = 0U;
        replyWithReceiveCalls = 0U;
        lastReplyControlId = 0U;
        nextReplyStatus = STATUS_SUCCESS;
        connectionCompleteDeliveries = 0U;
        connectElapsedTicks = 0;
        lastConnectTimeout = 0;
        lastEndpointReceiveTimeout = 0;
        nativeTypeHighBits = 0U;
        for (FakeAlpcEndpoint *endpoint : endpoints) {
            delete endpoint;
        }
        for (FakeAlpcServer *item : servers) {
            delete item;
        }
        endpoints.clear();
        servers.clear();
    }
};

static FakeAlpcState g_alpc = {};
static thread_local FakeAlpcQueued g_alpc_thread_current = {};
static LONGLONG g_alpc_system_time = 0;

static NTSTATUS NTAPI fake_alpc_query_system_time(PLARGE_INTEGER systemTime)
{
    if (systemTime == static_cast<PLARGE_INTEGER>(0)) {
        return STATUS_INVALID_PARAMETER;
    }
    systemTime->QuadPart = g_alpc_system_time;
    return STATUS_SUCCESS;
}

static FakeAlpcQueued make_alpc_message(const PALPC_PORT_MESSAGE message,
                                        FakeAlpcEndpoint *endpoint,
                                        FakeAlpcConnect *connect,
                                        FakeAlpcSync *sync)
{
    FakeAlpcQueued queued = {};
    SIZE_T length = static_cast<SIZE_T>(sizeof(ALPC_PORT_MESSAGE));

    queued.endpoint = endpoint;
    queued.connect = connect;
    queued.sync = sync;
    if (message != static_cast<PALPC_PORT_MESSAGE>(0)) {
        length = static_cast<SIZE_T>(message->u1.s1.TotalLength);
        if (length < static_cast<SIZE_T>(sizeof(ALPC_PORT_MESSAGE)) ||
            length > kAlpcCapacity) {
            length = static_cast<SIZE_T>(sizeof(ALPC_PORT_MESSAGE));
        }
        std::memcpy(queued.bytes.data(), message, static_cast<std::size_t>(length));
    }
    queued.length = length;
    return queued;
}

static FakeAlpcQueued make_alpc_connection_complete(
    FakeAlpcEndpoint *endpoint, USHORT messageType)
{
    FakeAlpcQueued queued = {};
    PALPC_PORT_MESSAGE message = reinterpret_cast<PALPC_PORT_MESSAGE>(
        queued.bytes.data());

    queued.endpoint = endpoint;
    queued.length = static_cast<SIZE_T>(sizeof(ALPC_PORT_MESSAGE));
    message->u1.s1.TotalLength =
        static_cast<USHORT>(sizeof(ALPC_PORT_MESSAGE));
    message->u1.s1.DataLength = 0U;
    message->u2.s2.Type = messageType;
    return queued;
}

static void enqueue_alpc_message(const FakeAlpcQueued &queued)
{
    {
        std::lock_guard<std::mutex> guard(g_alpc.mutex);
        g_alpc.queue.push_back(queued);
    }
    g_alpc.condition.notify_all();
}

static bool fake_alpc_wait_for_message(const LARGE_INTEGER *timeout)
{
    std::unique_lock<std::mutex> lock(g_alpc.mutex);

    if (!g_alpc.queue.empty()) {
        return true;
    }
    if (timeout == static_cast<const LARGE_INTEGER *>(0)) {
        g_alpc.condition.wait(lock, [] { return !g_alpc.queue.empty(); });
        return true;
    }
    if (timeout->QuadPart == 0) {
        return false;
    }
    if (timeout->QuadPart < 0) {
        const std::int64_t ticks = -timeout->QuadPart;
        const std::int64_t milliseconds = ticks / 10000;
        const std::int64_t bounded = milliseconds > 60000 ? 60000 : milliseconds;
        return g_alpc.condition.wait_for(
            lock, std::chrono::milliseconds(static_cast<long long>(bounded)),
            [] { return !g_alpc.queue.empty(); });
    }
    return false;
}

static NTSTATUS NTAPI fake_alpc_close(HANDLE handle)
{
    if (handle == static_cast<HANDLE>(0)) {
        return STATUS_INVALID_PARAMETER;
    }
    for (FakeAlpcServer *server : g_alpc.servers) {
        if (server == handle) {
            server->closed = true;
            return STATUS_SUCCESS;
        }
    }
    for (FakeAlpcEndpoint *endpoint : g_alpc.endpoints) {
        if (endpoint == handle) {
            endpoint->closed = true;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_INVALID_PARAMETER;
}

static void NTAPI fake_alpc_init_ansi(PANSI_STRING destination, PCSZ source)
{
    std::size_t length = source == static_cast<PCSZ>(0) ? 0U : std::strlen(source);

    if (destination == static_cast<PANSI_STRING>(0)) {
        return;
    }
    if (length > 0xffffU) {
        length = 0xffffU;
    }
    destination->Length = static_cast<USHORT>(length);
    destination->MaximumLength = static_cast<USHORT>(length + (length < 0xffffU ? 1U : 0U));
    destination->Buffer = const_cast<PCHAR>(source);
}

static NTSTATUS NTAPI fake_alpc_ansi_to_unicode(PUNICODE_STRING destination,
                                                 PANSI_STRING source,
                                                 BOOLEAN allocateDestination)
{
    std::size_t count = 0U;

    if (destination == static_cast<PUNICODE_STRING>(0) ||
        source == static_cast<PANSI_STRING>(0) || source->Buffer == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    count = source->Length;
    if (!allocateDestination) {
        return STATUS_NOT_SUPPORTED;
    }
    destination->Buffer = new wchar_t[count + 1U]();
    destination->Length = static_cast<USHORT>(count * sizeof(wchar_t));
    destination->MaximumLength = static_cast<USHORT>((count + 1U) * sizeof(wchar_t));
    for (std::size_t index = 0U; index < count; ++index) {
        destination->Buffer[index] = static_cast<unsigned char>(source->Buffer[index]);
    }
    return STATUS_SUCCESS;
}

static void NTAPI fake_alpc_free_unicode(PUNICODE_STRING string)
{
    if (string != static_cast<PUNICODE_STRING>(0)) {
        delete[] string->Buffer;
        string->Buffer = static_cast<wchar_t *>(0);
        string->Length = 0;
        string->MaximumLength = 0;
    }
}

static NTSTATUS NTAPI fake_alpc_create_port(PHANDLE portHandle,
                                             POBJECT_ATTRIBUTES,
                                             PALPC_PORT_SDK_ATTRIBUTES)
{
    FakeAlpcServer *server = new FakeAlpcServer();

    if (portHandle == static_cast<PHANDLE>(0)) {
        delete server;
        return STATUS_INVALID_PARAMETER;
    }
    g_alpc.servers.push_back(server);
    g_alpc.server = server;
    *portHandle = static_cast<HANDLE>(server);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI fake_alpc_accept_connect(PHANDLE portHandle,
                                                HANDLE,
                                                ULONG,
                                                POBJECT_ATTRIBUTES,
                                                PALPC_PORT_SDK_ATTRIBUTES,
                                                PVOID portContext,
                                                PALPC_PORT_MESSAGE connectionRequest,
                                                PVOID,
                                                BOOLEAN acceptConnection)
{
    FakeAlpcConnect *connect = g_alpc_thread_current.connect;

    if (connect == static_cast<FakeAlpcConnect *>(0) ||
        connectionRequest == static_cast<PALPC_PORT_MESSAGE>(0)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!acceptConnection) {
        connect->denied = true;
        return STATUS_SUCCESS;
    }
    FakeAlpcEndpoint *endpoint = new FakeAlpcEndpoint();
    endpoint->serverContext = portContext;
    g_alpc.endpoints.push_back(endpoint);
    connect->endpoint = endpoint;
    connect->responseControlId =
        static_cast<PALPC_PORT_FRAME>(static_cast<void *>(connectionRequest))->ControlId;
    connect->accepted = true;
    if (portHandle != static_cast<PHANDLE>(0)) {
        *portHandle = static_cast<HANDLE>(endpoint);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI fake_alpc_complete_connect(HANDLE)
{
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI fake_alpc_send_wait_receive(
    HANDLE portHandle, ULONG flags, PALPC_PORT_MESSAGE sendMessage, PVOID,
    PALPC_PORT_MESSAGE receiveMessage, PSIZE_T bufferLength,
    PVOID receiveMessageAttributes,
    PLARGE_INTEGER timeout);

static NTSTATUS fake_alpc_process_connection(void)
{
    if (g_alpc.serverContext == static_cast<ALPC_PORT_SERVER_CONTEXT *>(0)) {
        return STATUS_PORT_DISCONNECTED;
    }
    return AlpcPort_ProcessBlockedEvent(g_alpc.serverContext);
}

static NTSTATUS NTAPI fake_alpc_connect(PHANDLE portHandle,
                                        PCUNICODE_STRING,
                                        POBJECT_ATTRIBUTES,
                                        PALPC_PORT_SDK_ATTRIBUTES,
                                        ULONG,
                                        PSECURITY_DESCRIPTOR,
                                        PALPC_PORT_MESSAGE connectionMessage,
                                        PSIZE_T bufferLength,
                                        PVOID,
                                        PVOID,
                                        PLARGE_INTEGER timeout)
{
    FakeAlpcConnect connect = {};
    PALPC_PORT_FRAME frame = static_cast<PALPC_PORT_FRAME>(
        static_cast<void *>(connectionMessage));
    NTSTATUS processStatus = STATUS_UNSUCCESSFUL;

    if (portHandle == static_cast<PHANDLE>(0) ||
        g_alpc.server == static_cast<FakeAlpcServer *>(0) ||
        g_alpc.server->closed || frame == static_cast<PALPC_PORT_FRAME>(0)) {
        return STATUS_PORT_DISCONNECTED;
    }
    g_alpc.lastConnectTimeout = timeout != static_cast<PLARGE_INTEGER>(0)
                                    ? timeout->QuadPart
                                    : 0;
    {
        FakeAlpcQueued connectionRequest = make_alpc_message(
            &frame->header, static_cast<FakeAlpcEndpoint *>(0), &connect,
            static_cast<FakeAlpcSync *>(0));
        PALPC_PORT_MESSAGE nativeRequest =
            reinterpret_cast<PALPC_PORT_MESSAGE>(
                connectionRequest.bytes.data());
        nativeRequest->u2.s2.Type = static_cast<USHORT>(
            (nativeRequest->u2.s2.Type & 0x0fffU) |
            g_alpc.nativeTypeHighBits);
        enqueue_alpc_message(connectionRequest);
    }
    processStatus = fake_alpc_process_connection();
    if (!NT_SUCCESS(processStatus) || connect.denied || !connect.accepted ||
        connect.endpoint == static_cast<FakeAlpcEndpoint *>(0)) {
        return NT_SUCCESS(processStatus) && connect.denied
                   ? STATUS_PORT_DISCONNECTED
                   : processStatus;
    }
    *portHandle = static_cast<HANDLE>(connect.endpoint);
    g_alpc_system_time += g_alpc.connectElapsedTicks;
    if (g_alpc.emptyEndpointPollStatus == STATUS_TIMEOUT) {
        enqueue_alpc_message(make_alpc_connection_complete(
            connect.endpoint,
            static_cast<USHORT>(kAlpcConnectionCompleteType |
                                g_alpc.nativeTypeHighBits)));
    }
    /* Real NtAlpcConnectPort leaves the caller's connection payload in place;
       the ControlId chosen by the acceptor is not copied back to the client. */
    frame->Flags = 0;
    frame->PayloadLength = 0;
    frame->Status = STATUS_SUCCESS;
    frame->header.u2.s2.Type = static_cast<USHORT>(
        (ALPC_PORT_MESSAGE_TYPE_CONNECTION_REQUEST & 0x0fffU) |
        g_alpc.nativeTypeHighBits);
    frame->header.u1.s1.TotalLength = static_cast<USHORT>(ALPC_PORT_FRAME_DATA_OFFSET);
    frame->header.u1.s1.DataLength = static_cast<USHORT>(
        ALPC_PORT_FRAME_DATA_OFFSET - sizeof(ALPC_PORT_MESSAGE));
    /* Successful native calls may leave BufferLength at its input capacity.
       TotalLength is the authoritative number of returned bytes. */
    (void)bufferLength;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI fake_alpc_disconnect(HANDLE handle, ULONG)
{
    if (handle == static_cast<HANDLE>(0)) {
        return STATUS_INVALID_PARAMETER;
    }
    for (FakeAlpcServer *server : g_alpc.servers) {
        if (server == handle) {
            server->closed = true;
            return STATUS_SUCCESS;
        }
    }
    for (FakeAlpcEndpoint *endpoint : g_alpc.endpoints) {
        if (endpoint == handle) {
            endpoint->closed = true;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_INVALID_PARAMETER;
}

static NTSTATUS NTAPI fake_alpc_send_wait_receive(
    HANDLE portHandle, ULONG flags, PALPC_PORT_MESSAGE sendMessage, PVOID,
    PALPC_PORT_MESSAGE receiveMessage, PSIZE_T bufferLength,
    PVOID receiveMessageAttributes,
    PLARGE_INTEGER timeout)
{
    FakeAlpcEndpoint *endpoint = static_cast<FakeAlpcEndpoint *>(portHandle);
    bool serverHandle = false;

    if (portHandle != static_cast<HANDLE>(0)) {
        for (FakeAlpcServer *server : g_alpc.servers) {
            if (server == portHandle) {
                serverHandle = true;
                if (server->closed) {
                    return STATUS_PORT_DISCONNECTED;
                }
                break;
            }
        }
    }
    if (portHandle == static_cast<HANDLE>(0) ||
        (!serverHandle && (endpoint == static_cast<FakeAlpcEndpoint *>(0) ||
                           endpoint->closed))) {
        return STATUS_PORT_DISCONNECTED;
    }
    if (!serverHandle && sendMessage == static_cast<PALPC_PORT_MESSAGE>(0) &&
        receiveMessage != static_cast<PALPC_PORT_MESSAGE>(0)) {
        g_alpc.lastEndpointReceiveTimeout =
            timeout != static_cast<PLARGE_INTEGER>(0) ? timeout->QuadPart : 0;
        std::lock_guard<std::mutex> guard(g_alpc.mutex);
        if (g_alpc.queue.empty() &&
            g_alpc.emptyEndpointPollStatus != STATUS_TIMEOUT) {
            const NTSTATUS pollStatus = g_alpc.emptyEndpointPollStatus;
            g_alpc.emptyEndpointPollStatus = STATUS_TIMEOUT;
            return pollStatus;
        }
    }
    /* A receive-only call is used by both connection-port and communication
       port workers.  The caller identifies the queue entry by its endpoint. */
    if (sendMessage == static_cast<PALPC_PORT_MESSAGE>(0) &&
        receiveMessage != static_cast<PALPC_PORT_MESSAGE>(0)) {
        if (!fake_alpc_wait_for_message(timeout)) {
            return STATUS_TIMEOUT;
        }
        {
            std::lock_guard<std::mutex> guard(g_alpc.mutex);
            if (g_alpc.queue.empty()) {
                return STATUS_TIMEOUT;
            }
            FakeAlpcQueued selected = g_alpc.queue.front();
            if (!serverHandle && endpoint != static_cast<FakeAlpcEndpoint *>(0) &&
                selected.endpoint != endpoint) {
                return STATUS_TIMEOUT;
            }
            g_alpc.queue.pop_front();
            g_alpc.current = selected;
            g_alpc.hasCurrent = true;
            if (selected.endpoint != static_cast<FakeAlpcEndpoint *>(0)) {
                selected.endpoint->pendingSync = selected.sync;
            }
        }
        g_alpc_thread_current = g_alpc.current;
        if (bufferLength == static_cast<PSIZE_T>(0) ||
            *bufferLength < g_alpc.current.length) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        std::memset(receiveMessage, 0, static_cast<std::size_t>(*bufferLength));
        std::memcpy(receiveMessage, g_alpc.current.bytes.data(),
                    static_cast<std::size_t>(g_alpc.current.length));
        if (!serverHandle &&
            (receiveMessage->u2.s2.Type & 0x0fffU) ==
                kAlpcConnectionCompleteType) {
            std::lock_guard<std::mutex> guard(g_alpc.mutex);
            ++g_alpc.connectionCompleteDeliveries;
        }
        /* Match native ALPC: BufferLength is an in/out capacity and is not
           guaranteed to be rewritten on success. */
        if (receiveMessageAttributes != static_cast<PVOID>(0) &&
            g_alpc_thread_current.endpoint !=
                static_cast<FakeAlpcEndpoint *>(0)) {
            PALPC_PORT_CONTEXT_ATTRIBUTES attributes =
                static_cast<PALPC_PORT_CONTEXT_ATTRIBUTES>(
                    receiveMessageAttributes);
            if ((attributes->Header.AllocatedAttributes &
                 ALPC_PORT_MESSAGE_CONTEXT_ATTRIBUTE) != 0U) {
                attributes->Header.ValidAttributes |=
                    ALPC_PORT_MESSAGE_CONTEXT_ATTRIBUTE;
                attributes->Context.PortContext =
                    g_alpc.portContextOverride != static_cast<PVOID>(0)
                        ? g_alpc.portContextOverride
                        : g_alpc_thread_current.endpoint->serverContext;
                attributes->Context.MessageId = receiveMessage->MessageId;
            }
        }
        return STATUS_SUCCESS;
    }
    /* A reply is sent from ProcessClientEvent after a request was received.
       The current thread's queue metadata identifies the waiting sender. */
    if ((flags & static_cast<ULONG>(ALPC_PORT_SEND_FLAG_REPLY_MESSAGE)) != 0U &&
        sendMessage != static_cast<PALPC_PORT_MESSAGE>(0)) {
        FakeAlpcSync *sync = static_cast<FakeAlpcSync *>(0);
        const SIZE_T length = static_cast<SIZE_T>(sendMessage->u1.s1.TotalLength);
        NTSTATUS injectedReplyStatus = STATUS_SUCCESS;

        {
            std::lock_guard<std::mutex> guard(g_alpc.mutex);
            sync = g_alpc_thread_current.sync !=
                           static_cast<FakeAlpcSync *>(0)
                       ? g_alpc_thread_current.sync
                       : endpoint->pendingSync;
            if (endpoint->pendingSync == sync) {
                endpoint->pendingSync = static_cast<FakeAlpcSync *>(0);
            }
            if (receiveMessage == static_cast<PALPC_PORT_MESSAGE>(0) &&
                bufferLength == static_cast<PSIZE_T>(0)) {
                ++g_alpc.replySendOnlyCalls;
            } else {
                ++g_alpc.replyWithReceiveCalls;
            }
            g_alpc.lastReplyControlId =
                static_cast<PALPC_PORT_FRAME>(
                    static_cast<void *>(sendMessage))->ControlId;
            injectedReplyStatus = g_alpc.nextReplyStatus;
            g_alpc.nextReplyStatus = STATUS_SUCCESS;
        }
        if (sync == static_cast<FakeAlpcSync *>(0)) {
            return STATUS_INVALID_PARAMETER;
        }
        if (injectedReplyStatus != STATUS_SUCCESS) {
            return injectedReplyStatus;
        }
        if (length < static_cast<SIZE_T>(sizeof(ALPC_PORT_MESSAGE)) ||
            length > kAlpcCapacity) {
            return STATUS_DATA_ERROR;
        }
        {
            std::lock_guard<std::mutex> guard(sync->mutex);
            std::memcpy(sync->reply.data(), sendMessage, static_cast<std::size_t>(length));
            reinterpret_cast<PALPC_PORT_MESSAGE>(
                sync->reply.data())->u2.s2.Type = static_cast<USHORT>(
                    (sendMessage->u2.s2.Type & 0x0fffU) |
                    g_alpc.nativeTypeHighBits);
            sync->replyLength = length;
            sync->status = STATUS_SUCCESS;
            sync->replied = true;
        }
        sync->condition.notify_all();
        return STATUS_SUCCESS;
    }
    /* Client datagram/request send.  A malformed frame is injected before
       the server receives it to exercise the SDK's defensive reply path. */
    if (sendMessage == static_cast<PALPC_PORT_MESSAGE>(0)) {
        return STATUS_INVALID_PARAMETER;
    }
    FakeAlpcSync *sync =
        receiveMessage != static_cast<PALPC_PORT_MESSAGE>(0)
            ? new FakeAlpcSync()
            : static_cast<FakeAlpcSync *>(0);
    FakeAlpcQueued queued = make_alpc_message(sendMessage, endpoint,
                                               static_cast<FakeAlpcConnect *>(0), sync);
    reinterpret_cast<PALPC_PORT_MESSAGE>(queued.bytes.data())->u2.s2.Type =
        static_cast<USHORT>(sync != static_cast<FakeAlpcSync *>(0)
                                ? (ALPC_PORT_MESSAGE_TYPE_REQUEST & 0x0fffU) |
                                      g_alpc.nativeTypeHighBits
                                : (ALPC_PORT_MESSAGE_TYPE_DATAGRAM & 0x0fffU) |
                                      g_alpc.nativeTypeHighBits);
    if (g_alpc.mutateNextRequest) {
        g_alpc.mutateNextRequest = false;
        queued.bytes[0] = 0;
        queued.bytes[1] = 0;
        queued.bytes[2] = 0;
        queued.bytes[3] = static_cast<unsigned char>(sizeof(ALPC_PORT_MESSAGE) - 1U);
    }
    if (g_alpc.appendTrailingByteNextRequest &&
        queued.length < kAlpcCapacity) {
        PALPC_PORT_MESSAGE malformed =
            reinterpret_cast<PALPC_PORT_MESSAGE>(queued.bytes.data());
        g_alpc.appendTrailingByteNextRequest = false;
        queued.bytes[queued.length] = 0xA5U;
        ++queued.length;
        ++malformed->u1.s1.TotalLength;
        ++malformed->u1.s1.DataLength;
    }
    enqueue_alpc_message(queued);
    if (sync != static_cast<FakeAlpcSync *>(0)) {
        const NTSTATUS processStatus = AlpcPort_ProcessClientEvent(
            g_alpc.serverContext, static_cast<HANDLE>(endpoint));
        if ((processStatus == STATUS_TIMEOUT || !NT_SUCCESS(processStatus)) &&
            !sync->replied) {
            delete sync;
            return processStatus;
        }
        {
            std::unique_lock<std::mutex> lock(sync->mutex);
            sync->condition.wait(lock, [sync] { return sync->replied; });
        }
        if (receiveMessage != static_cast<PALPC_PORT_MESSAGE>(0) &&
            bufferLength != static_cast<PSIZE_T>(0)) {
            if (*bufferLength < sync->replyLength) {
                delete sync;
                return STATUS_INFO_LENGTH_MISMATCH;
            }
            std::memset(receiveMessage, 0, static_cast<std::size_t>(*bufferLength));
            std::memcpy(receiveMessage, sync->reply.data(),
                        static_cast<std::size_t>(sync->replyLength));
            /* Preserve the caller-provided capacity, as the real syscall may. */
        }
        const NTSTATUS result = sync->status;
        delete sync;
        return result;
    }
    return STATUS_SUCCESS;
}

static ALPC_PORT_CLIENT_APIS fake_alpc_client_apis(void)
{
    ALPC_PORT_CLIENT_APIS api = {};
    api.pfnRtlInitAnsiString = fake_alpc_init_ansi;
    api.pfnRtlAnsiStringToUnicodeString = fake_alpc_ansi_to_unicode;
    api.pfnRtlFreeUnicodeString = fake_alpc_free_unicode;
    api.pfnNtClose = fake_alpc_close;
    api.pfnNtAlpcConnectPort = fake_alpc_connect;
    api.pfnNtAlpcSendWaitReceivePort = fake_alpc_send_wait_receive;
    api.pfnNtAlpcDisconnectPort = fake_alpc_disconnect;
    api.pfnNtQuerySystemTime = fake_alpc_query_system_time;
    return api;
}

static ALPC_PORT_SERVER_APIS fake_alpc_server_apis(void)
{
    ALPC_PORT_SERVER_APIS api = {};
    api.pfnRtlInitAnsiString = fake_alpc_init_ansi;
    api.pfnRtlAnsiStringToUnicodeString = fake_alpc_ansi_to_unicode;
    api.pfnRtlFreeUnicodeString = fake_alpc_free_unicode;
    api.pfnNtClose = fake_alpc_close;
    api.pfnNtAlpcCreatePort = fake_alpc_create_port;
    api.pfnNtAlpcAcceptConnectPort = fake_alpc_accept_connect;
    api.pfnNtAlpcCompleteConnectPort = fake_alpc_complete_connect;
    api.pfnNtAlpcSendWaitReceivePort = fake_alpc_send_wait_receive;
    api.pfnNtAlpcDisconnectPort = fake_alpc_disconnect;
    api.pfnNtQuerySystemTime = fake_alpc_query_system_time;
    return api;
}

struct AlpcCallbackState
{
    uint32_t preConnectCalls;
    uint32_t postConnectCalls;
    uint32_t syncCalls;
    uint32_t asyncCalls;
    uint32_t closeCalls;
    uint32_t lastControlId;
    NTSTATUS syncStatus;
    NTSTATUS asyncStatus;
    std::vector<unsigned char> lastAsync;

    AlpcCallbackState()
        : preConnectCalls(0U), postConnectCalls(0U), syncCalls(0U),
          asyncCalls(0U), closeCalls(0U), lastControlId(0U),
          syncStatus(STATUS_SUCCESS), asyncStatus(STATUS_SUCCESS), lastAsync()
    {
    }
};

static AlpcCallbackState *g_alpc_callbacks = static_cast<AlpcCallbackState *>(0);

struct DeferredReplyState
{
    ALPC_PORT_SERVER_CONTEXT *server;
    ALPC_PORT_REPLY_TOKEN token;
    std::mutex mutex;
    std::condition_variable condition;
    NTSTATUS captureStatus;
    NTSTATUS replyStatus;
    NTSTATUS duplicateStatus;
    bool captured;

    DeferredReplyState()
        : server(static_cast<ALPC_PORT_SERVER_CONTEXT *>(0)), token(), mutex(),
          condition(), captureStatus(STATUS_UNSUCCESSFUL),
          replyStatus(STATUS_UNSUCCESSFUL),
          duplicateStatus(STATUS_UNSUCCESSFUL), captured(false)
    {
    }
};

static DeferredReplyState *g_deferred_reply =
    static_cast<DeferredReplyState *>(0);

static NTSTATUS alpc_deferred_request(HANDLE clientPort,
                                      const ALPC_PORT_CLIENT_ID *,
                                      uint32_t, uint8_t *, ULONG *, ULONG,
                                      PVOID)
{
    DeferredReplyState *state = g_deferred_reply;
    if (state == static_cast<DeferredReplyState *>(0) ||
        state->server == static_cast<ALPC_PORT_SERVER_CONTEXT *>(0)) {
        return STATUS_INVALID_PARAMETER;
    }
    state->captureStatus = AlpcPort_ServerCaptureRequest(
        state->server, clientPort, &state->token);
    {
        std::lock_guard<std::mutex> guard(state->mutex);
        state->captured = true;
    }
    state->condition.notify_all();
    return state->captureStatus == STATUS_SUCCESS
               ? STATUS_PENDING : state->captureStatus;
}

static void alpc_pre_connect(uint32_t *responseControlId, uint8_t *deny, PVOID)
{
    if (g_alpc_callbacks != static_cast<AlpcCallbackState *>(0)) {
        ++g_alpc_callbacks->preConnectCalls;
    }
    if (responseControlId != static_cast<uint32_t *>(0)) {
        *responseControlId += 0x20U;
    }
    if (deny != static_cast<uint8_t *>(0)) {
        *deny = 0U;
    }
}

static void alpc_post_connect(HANDLE, const ALPC_PORT_CLIENT_ID *,
                              uint32_t controlId, PVOID)
{
    if (g_alpc_callbacks != static_cast<AlpcCallbackState *>(0)) {
        ++g_alpc_callbacks->postConnectCalls;
        g_alpc_callbacks->lastControlId = controlId;
    }
}

static NTSTATUS alpc_sync_request(HANDLE, const ALPC_PORT_CLIENT_ID *,
                                   uint32_t controlId, uint8_t *payload,
                                   ULONG *payloadLength, ULONG, PVOID)
{
    if (g_alpc_callbacks != static_cast<AlpcCallbackState *>(0)) {
        ++g_alpc_callbacks->syncCalls;
        g_alpc_callbacks->lastControlId = controlId;
        if (payload != static_cast<uint8_t *>(0) && payloadLength != static_cast<ULONG *>(0) &&
            *payloadLength != 0U) {
            payload[0] ^= 0x20U;
            payload[*payloadLength - 1U] ^= 0x20U;
        }
        return g_alpc_callbacks->syncStatus;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS alpc_async_request(HANDLE, const ALPC_PORT_CLIENT_ID *,
                                   uint32_t, const uint8_t *payload,
                                   ULONG payloadLength, PVOID)
{
    if (g_alpc_callbacks != static_cast<AlpcCallbackState *>(0)) {
        ++g_alpc_callbacks->asyncCalls;
        g_alpc_callbacks->lastAsync.assign(payload, payload + payloadLength);
    }
    return g_alpc_callbacks != static_cast<AlpcCallbackState *>(0)
               ? g_alpc_callbacks->asyncStatus
               : STATUS_SUCCESS;
}

static void alpc_close(HANDLE, const ALPC_PORT_CLIENT_ID *, PVOID)
{
    if (g_alpc_callbacks != static_cast<AlpcCallbackState *>(0)) {
        ++g_alpc_callbacks->closeCalls;
    }
}

static void alpc_fill_name(ALPC_PORT_NAME *name)
{
    const char value[] = "\\RPC Control\\UnitAlpc";
    if (name != static_cast<ALPC_PORT_NAME *>(0)) {
        std::memset(name, 0, sizeof(*name));
        std::memcpy(name->name, value, sizeof(value));
    }
}

struct AlpcFixture
{
    ALPC_PORT_SERVER_CONTEXT server;
    ALPC_PORT_CLIENT_CONTEXT client;
    ALPC_PORT_SERVER_CONFIG serverConfig;
    ALPC_PORT_CLIENT_CONFIG clientConfig;
    ALPC_PORT_SERVER_EVENTS events;
    AlpcCallbackState callbacks;

    AlpcFixture(USHORT nativeTypeHighBits = 0U)
        : server(), client(), serverConfig(), clientConfig(), events(),
          callbacks()
    {
        g_alpc.nativeTypeHighBits = nativeTypeHighBits;
        alpc_fill_name(&serverConfig.portName);
        serverConfig.maxMessageLength = kAlpcCapacity;
        serverConfig.maxClients = 8U;
        serverConfig.api = fake_alpc_server_apis();
        alpc_fill_name(&clientConfig.portName);
        clientConfig.helloId = 0x33U;
        clientConfig.maxMessageLength = kAlpcCapacity;
        clientConfig.api = fake_alpc_client_apis();
        events.onPreConnect = alpc_pre_connect;
        events.onPostConnect = alpc_post_connect;
        events.onSyncRequest = alpc_sync_request;
        events.onAsyncRequest = alpc_async_request;
        events.onClose = alpc_close;
        g_alpc_callbacks = &callbacks;
        REQUIRE(AlpcPort_ServerCreate(&serverConfig, &server) == STATUS_SUCCESS);
        g_alpc.serverContext = &server;
        REQUIRE(AlpcPort_Register_ServerEvtCallback(&server, &events) != 0U);
        REQUIRE(AlpcPort_Connect(&clientConfig, &client) == STATUS_SUCCESS);
    }

    ~AlpcFixture()
    {
        AlpcPort_Disconnect(&client);
        AlpcPort_ServerClose(&server);
        g_alpc_callbacks = static_cast<AlpcCallbackState *>(0);
        g_deferred_reply = static_cast<DeferredReplyState *>(0);
        g_alpc.reset();
    }

    AlpcFixture(const AlpcFixture &) = delete;
    AlpcFixture &operator=(const AlpcFixture &) = delete;
};

struct ReplyCapture
{
    std::vector<unsigned char> bytes;
    uint32_t calls;

    ReplyCapture() : bytes(), calls(0U)
    {
    }
};

static NTSTATUS NTAPI alpc_reply_capture(const uint8_t *buffer, ULONG length,
                                         PVOID context)
{
    ReplyCapture *capture = static_cast<ReplyCapture *>(context);

    if (capture == static_cast<ReplyCapture *>(0)) {
        return STATUS_INVALID_PARAMETER;
    }
    ++capture->calls;
    capture->bytes.clear();
    if (buffer != static_cast<const uint8_t *>(0) && length != 0U) {
        capture->bytes.assign(buffer, buffer + length);
    }
    return STATUS_SUCCESS;
}

} /* namespace */

TEST_CASE("ALPC validates configuration and supports timed connection waits",
          "[alpc][validation]")
{
    ALPC_PORT_SERVER_CONTEXT server = {};
    ALPC_PORT_SERVER_CONFIG config = {};
    LARGE_INTEGER timeout = {};

    REQUIRE(AlpcPort_ServerCreate(static_cast<PALPC_PORT_SERVER_CONFIG>(0),
                                   &server) == STATUS_INVALID_PARAMETER);
    alpc_fill_name(&config.portName);
    config.maxMessageLength = kAlpcCapacity;
    config.api = fake_alpc_server_apis();
    REQUIRE(AlpcPort_ServerCreate(&config, &server) == STATUS_SUCCESS);
    g_alpc.serverContext = &server;
    timeout.QuadPart = 0;
    REQUIRE(AlpcPort_ProcessBlockedEventEx(&server, &timeout) == STATUS_TIMEOUT);
    timeout.QuadPart = -10000;
    REQUIRE(AlpcPort_ProcessBlockedEventEx(&server, &timeout) == STATUS_TIMEOUT);
    AlpcPort_ServerClose(&server);
    g_alpc.reset();
}

TEST_CASE("ALPC performs synchronous and asynchronous exchanges", "[alpc][transport]")
{
    AlpcFixture fixture = {};
    ReplyCapture capture = {};
    const unsigned char inlineMessage[] = {'h', 'E', 'l', 'L', 'o'};
    const unsigned char asyncMessage[] = {'a', 's', 'y', 'n', 'c'};
    const std::vector<unsigned char> expectedInline = {'H', 'E', 'l', 'L', 'O'};

    REQUIRE(g_alpc.connectionCompleteDeliveries == 1U);

    REQUIRE(AlpcPort_SendMessage(&fixture.client, inlineMessage,
                                 static_cast<ULONG>(sizeof(inlineMessage)), 7U,
                                 0U, alpc_reply_capture, &capture, nullptr) ==
            STATUS_SUCCESS);
    REQUIRE(capture.calls == 1U);
    REQUIRE(capture.bytes == expectedInline);
    REQUIRE(fixture.callbacks.syncCalls == 1U);
    REQUIRE(fixture.callbacks.lastControlId == 7U);
    REQUIRE(g_alpc.replySendOnlyCalls == 1U);
    REQUIRE(g_alpc.replyWithReceiveCalls == 0U);
    REQUIRE(g_alpc.lastReplyControlId == 7U);

    REQUIRE(AlpcPort_SendMessage(&fixture.client, asyncMessage,
                                 static_cast<ULONG>(sizeof(asyncMessage)), 9U,
                                 ALPC_PORT_SEND_FLAG_ASYNC,
                                 static_cast<AlpcPort_SyncReplyCallback>(0),
                                 static_cast<PVOID>(0), nullptr) == STATUS_SUCCESS);
    REQUIRE(AlpcPort_ProcessClientEvent(&fixture.server,
                                         fixture.client.portHandle) == STATUS_SUCCESS);
    REQUIRE(fixture.callbacks.asyncCalls == 1U);
    REQUIRE(fixture.callbacks.lastAsync.size() == sizeof(asyncMessage));
}

TEST_CASE("ALPC accepts WOW64 Native message type flags",
          "[alpc][transport][wow64]")
{
    AlpcFixture fixture(static_cast<USHORT>(0x3000U));
    ReplyCapture capture = {};
    const unsigned char syncMessage[] = {'w', 'o', 'w'};
    const unsigned char asyncMessage[] = {'6', '4'};
    const std::vector<unsigned char> expectedReply = {'W', 'o', 'W'};
    ALPC_PORT_MESSAGE nativeControl = {};

    /* The fake provider applies 0x3000 to the connection request, completion
       marker, request/datagram and reply types, matching observed WOW64 ALPC
       traffic rather than only exercising a helper in isolation. */
    REQUIRE(g_alpc.connectionCompleteDeliveries == 1U);
    REQUIRE(AlpcPort_SendMessage(
                &fixture.client, syncMessage,
                static_cast<ULONG>(sizeof(syncMessage)), 0x64U, 0U,
                alpc_reply_capture, &capture, nullptr) == STATUS_SUCCESS);
    REQUIRE(capture.bytes == expectedReply);
    REQUIRE(fixture.callbacks.syncCalls == 1U);

    REQUIRE(AlpcPort_SendMessage(
                &fixture.client, asyncMessage,
                static_cast<ULONG>(sizeof(asyncMessage)), 0x65U,
                ALPC_PORT_SEND_FLAG_ASYNC,
                static_cast<AlpcPort_SyncReplyCallback>(0),
                static_cast<PVOID>(0), nullptr) == STATUS_SUCCESS);
    REQUIRE(AlpcPort_ProcessBlockedEvent(&fixture.server) == STATUS_SUCCESS);
    REQUIRE(fixture.callbacks.asyncCalls == 1U);

    nativeControl.u1.s1.TotalLength =
        static_cast<USHORT>(sizeof(ALPC_PORT_MESSAGE));
    nativeControl.u1.s1.DataLength = 0U;
    nativeControl.u2.s2.Type = static_cast<USHORT>(0x300CU);
    enqueue_alpc_message(make_alpc_message(
        &nativeControl, static_cast<FakeAlpcEndpoint *>(
                            fixture.client.portHandle),
        static_cast<FakeAlpcConnect *>(0), static_cast<FakeAlpcSync *>(0)));
    REQUIRE(AlpcPort_ProcessBlockedEvent(&fixture.server) == STATUS_SUCCESS);
    REQUIRE(fixture.server.clientCount == 1U);
}

TEST_CASE("ALPC connection completion consumes the remaining connect deadline",
          "[alpc][connect][timeout]")
{
    ALPC_PORT_SERVER_CONTEXT server = {};
    ALPC_PORT_CLIENT_CONTEXT client = {};
    ALPC_PORT_SERVER_CONFIG serverConfig = {};
    ALPC_PORT_CLIENT_CONFIG clientConfig = {};
    ALPC_PORT_SERVER_EVENTS events = {};
    AlpcCallbackState callbacks = {};
    LARGE_INTEGER timeout = {};

    g_alpc.reset();
    g_alpc_system_time = 1000;
    g_alpc.connectElapsedTicks = 400;
    timeout.QuadPart = -1000;
    alpc_fill_name(&serverConfig.portName);
    serverConfig.maxMessageLength = kAlpcCapacity;
    serverConfig.maxClients = 1U;
    serverConfig.api = fake_alpc_server_apis();
    alpc_fill_name(&clientConfig.portName);
    clientConfig.maxMessageLength = kAlpcCapacity;
    clientConfig.connectTimeout = &timeout;
    clientConfig.api = fake_alpc_client_apis();
    events.onPreConnect = alpc_pre_connect;
    events.onPostConnect = alpc_post_connect;
    g_alpc_callbacks = &callbacks;

    REQUIRE(AlpcPort_ServerCreate(&serverConfig, &server) == STATUS_SUCCESS);
    g_alpc.serverContext = &server;
    REQUIRE(AlpcPort_Register_ServerEvtCallback(&server, &events) != 0U);
    REQUIRE(AlpcPort_Connect(&clientConfig, &client) == STATUS_SUCCESS);
    REQUIRE(g_alpc.lastConnectTimeout == -1000);
    REQUIRE(g_alpc.lastEndpointReceiveTimeout == -600);
    REQUIRE(g_alpc.connectionCompleteDeliveries == 1U);

    AlpcPort_Disconnect(&client);
    AlpcPort_ServerClose(&server);
    g_alpc_callbacks = static_cast<AlpcCallbackState *>(0);
    g_alpc.reset();
    g_alpc_system_time = 0;
}

TEST_CASE("ALPC disconnects an endpoint when a synchronous reply cannot be delivered",
          "[alpc][reply][failure]")
{
    AlpcFixture fixture = {};
    const unsigned char message[] = {'r', 'e', 'p', 'l', 'y'};

    g_alpc.nextReplyStatus = STATUS_TIMEOUT;
    REQUIRE(AlpcPort_SendMessage(&fixture.client, message,
                                 static_cast<ULONG>(sizeof(message)), 0x71U,
                                 0U, nullptr, nullptr, nullptr) ==
            STATUS_TIMEOUT);
    REQUIRE(fixture.server.clientCount == 0U);
    REQUIRE(fixture.callbacks.closeCalls == 1U);
    REQUIRE(!g_alpc.endpoints.empty());
    REQUIRE(g_alpc.endpoints.front()->closed);
}

TEST_CASE("ALPC propagates callback failure and rejects malformed requests",
          "[alpc][validation][malformed]")
{
    AlpcFixture fixture = {};
    ReplyCapture capture = {};
    const unsigned char message[] = {'x', 'y'};

    fixture.callbacks.syncStatus = STATUS_DATA_ERROR;
    REQUIRE(AlpcPort_SendMessage(&fixture.client, message,
                                 static_cast<ULONG>(sizeof(message)), 2U, 0U,
                                 alpc_reply_capture, &capture, nullptr) ==
            STATUS_DATA_ERROR);
    REQUIRE(capture.calls == 1U);
    REQUIRE(capture.bytes.size() == sizeof(message));

    fixture.callbacks.syncStatus = STATUS_SUCCESS;
    g_alpc.mutateNextRequest = true;
    capture = ReplyCapture();
    REQUIRE(AlpcPort_SendMessage(&fixture.client, message,
                                 static_cast<ULONG>(sizeof(message)), 3U, 0U,
                                 alpc_reply_capture, &capture, nullptr) ==
            STATUS_DATA_ERROR);
    REQUIRE(capture.calls == 1U);
    REQUIRE(capture.bytes.empty());

    g_alpc.appendTrailingByteNextRequest = true;
    capture = ReplyCapture();
    REQUIRE(AlpcPort_SendMessage(&fixture.client, message,
                                 static_cast<ULONG>(sizeof(message)), 4U, 0U,
                                 alpc_reply_capture, &capture, nullptr) ==
            STATUS_DATA_ERROR);
    REQUIRE(capture.calls == 1U);
    REQUIRE(capture.bytes.empty());
}

TEST_CASE("ALPC timeout helpers preserve one absolute deadline",
          "[alpc][timeout]")
{
    LARGE_INTEGER timeout = {};
    LARGE_INTEGER deadline = {};
    LARGE_INTEGER remaining = {};

    timeout.QuadPart = 0;
    deadline.QuadPart = 1;
    REQUIRE(AlpcPort_NormalizeTimeout(fake_alpc_query_system_time, &timeout,
                                      &deadline) == STATUS_TIMEOUT);
    REQUIRE(deadline.QuadPart == 0);
    remaining.QuadPart = 1;
    REQUIRE(AlpcPort_RemainingTimeout(fake_alpc_query_system_time, &deadline,
                                      &remaining) == STATUS_TIMEOUT);
    REQUIRE(remaining.QuadPart == 0);

    g_alpc_system_time = 1000;
    timeout.QuadPart = -500;
    REQUIRE(AlpcPort_NormalizeTimeout(fake_alpc_query_system_time, &timeout,
                                      &deadline) == STATUS_SUCCESS);
    REQUIRE(deadline.QuadPart == 1500);
    g_alpc_system_time = 1200;
    REQUIRE(AlpcPort_RemainingTimeout(fake_alpc_query_system_time, &deadline,
                                      &remaining) == STATUS_SUCCESS);
    REQUIRE(remaining.QuadPart == -300);
    g_alpc_system_time = 1500;
    REQUIRE(AlpcPort_RemainingTimeout(fake_alpc_query_system_time, &deadline,
                                      &remaining) == STATUS_TIMEOUT);
    REQUIRE(remaining.QuadPart == 0);

    timeout.QuadPart = (std::numeric_limits<LONGLONG>::min)();
    REQUIRE(AlpcPort_NormalizeTimeout(fake_alpc_query_system_time, &timeout,
                                      &deadline) == STATUS_INTEGER_OVERFLOW);
    g_alpc_system_time = (std::numeric_limits<LONGLONG>::max)() - 10;
    timeout.QuadPart = -20;
    REQUIRE(AlpcPort_NormalizeTimeout(fake_alpc_query_system_time, &timeout,
                                      &deadline) == STATUS_INTEGER_OVERFLOW);
}

TEST_CASE("ALPC shared queue validates native PortContext",
          "[alpc][routing]")
{
    AlpcFixture fixture = {};
    FakeAlpcEndpoint *endpoint = static_cast<FakeAlpcEndpoint *>(
        fixture.client.portHandle);
    const std::uintptr_t nativeToken = reinterpret_cast<std::uintptr_t>(
        endpoint->serverContext);
    const std::uintptr_t invalidToken =
        nativeToken == (std::numeric_limits<std::uintptr_t>::max)()
            ? nativeToken - 1U
            : nativeToken + 1U;
    const unsigned char message[] = {'r', 'o', 'u', 't', 'e'};

    REQUIRE(AlpcPort_SendMessage(&fixture.client, message,
                                 static_cast<ULONG>(sizeof(message)), 0x31U,
                                 ALPC_PORT_SEND_FLAG_ASYNC, nullptr, nullptr,
                                 nullptr) == STATUS_SUCCESS);
    REQUIRE(AlpcPort_ProcessBlockedEvent(&fixture.server) == STATUS_SUCCESS);
    REQUIRE(fixture.callbacks.asyncCalls == 1U);

    g_alpc.portContextOverride = reinterpret_cast<PVOID>(invalidToken);
    REQUIRE(AlpcPort_SendMessage(&fixture.client, message,
                                 static_cast<ULONG>(sizeof(message)), 0x32U,
                                 ALPC_PORT_SEND_FLAG_ASYNC, nullptr, nullptr,
                                 nullptr) == STATUS_SUCCESS);
    REQUIRE(AlpcPort_ProcessBlockedEvent(&fixture.server) == STATUS_DATA_ERROR);
    REQUIRE(fixture.callbacks.asyncCalls == 1U);
    g_alpc.portContextOverride = static_cast<PVOID>(0);
}

TEST_CASE("ALPC native PortContext survives same-process endpoint ABA",
          "[alpc][routing][lifetime][token]")
{
    AlpcFixture fixture = {};
    ALPC_PORT_CLIENT_CONTEXT replacement = {};
    ALPC_PORT_ENDPOINT_TOKEN firstPublicToken = {};
    ALPC_PORT_ENDPOINT_TOKEN replacementPublicToken = {};
    FakeAlpcEndpoint *firstEndpoint = static_cast<FakeAlpcEndpoint *>(
        fixture.client.portHandle);
    FakeAlpcEndpoint *replacementEndpoint = nullptr;
    PVOID staleNativeToken = firstEndpoint->serverContext;
    const unsigned char message[] = {'a', 'b', 'a'};

    REQUIRE(staleNativeToken != nullptr);
    REQUIRE(AlpcPort_ServerGetClientToken(
                &fixture.server, fixture.client.portHandle,
                &firstPublicToken) == STATUS_SUCCESS);
    /* Native must retain an opaque scalar, never the freeable record address
       exposed as the opaque half of the public endpoint token. */
    REQUIRE(staleNativeToken != firstPublicToken.opaque);

    REQUIRE(AlpcPort_ServerDisconnectClientToken(&fixture.server,
                                                  &firstPublicToken) ==
            STATUS_SUCCESS);
    REQUIRE(AlpcPort_Connect(&fixture.clientConfig, &replacement) ==
            STATUS_SUCCESS);
    replacementEndpoint = static_cast<FakeAlpcEndpoint *>(
        replacement.portHandle);
    REQUIRE(replacementEndpoint->serverContext != nullptr);
    REQUIRE(replacementEndpoint->serverContext != staleNativeToken);
    REQUIRE(AlpcPort_ServerGetClientToken(
                &fixture.server, replacement.portHandle,
                &replacementPublicToken) == STATUS_SUCCESS);
    REQUIRE(replacementEndpoint->serverContext !=
            replacementPublicToken.opaque);

    /* A delayed frame from the disconnected endpoint retains its old token.
       Both fake endpoints use the same zero ClientId, so ClientId validation
       cannot accidentally make this regression pass. */
    g_alpc.portContextOverride = staleNativeToken;
    REQUIRE(AlpcPort_SendMessage(
                &replacement, message, static_cast<ULONG>(sizeof(message)),
                0x34U, ALPC_PORT_SEND_FLAG_ASYNC, nullptr, nullptr,
                nullptr) == STATUS_SUCCESS);
    REQUIRE(AlpcPort_ProcessBlockedEvent(&fixture.server) == STATUS_DATA_ERROR);
    REQUIRE(fixture.callbacks.asyncCalls == 0U);

    /* Model the address-reuse half of the ABA explicitly: an old pointer
       context whose numeric value now equals the replacement record address
       must still not route to that record. */
    g_alpc.portContextOverride = replacementPublicToken.opaque;
    REQUIRE(AlpcPort_SendMessage(
                &replacement, message, static_cast<ULONG>(sizeof(message)),
                0x35U, ALPC_PORT_SEND_FLAG_ASYNC, nullptr, nullptr,
                nullptr) == STATUS_SUCCESS);
    REQUIRE(AlpcPort_ProcessBlockedEvent(&fixture.server) == STATUS_DATA_ERROR);
    REQUIRE(fixture.callbacks.asyncCalls == 0U);

    g_alpc.portContextOverride = static_cast<PVOID>(0);
    REQUIRE(AlpcPort_SendMessage(
                &replacement, message, static_cast<ULONG>(sizeof(message)),
                0x36U, ALPC_PORT_SEND_FLAG_ASYNC, nullptr, nullptr,
                nullptr) == STATUS_SUCCESS);
    REQUIRE(AlpcPort_ProcessBlockedEvent(&fixture.server) == STATUS_SUCCESS);
    REQUIRE(fixture.callbacks.asyncCalls == 1U);

    REQUIRE(AlpcPort_ServerDisconnectClientToken(
                &fixture.server, &replacementPublicToken) == STATUS_SUCCESS);
    AlpcPort_Disconnect(&replacement);
}

TEST_CASE("ALPC shared queue distinguishes callback status from native close",
          "[alpc][routing][lifetime]")
{
    AlpcFixture fixture = {};
    const unsigned char message[] = {'s', 't', 'a', 'y'};
    FakeAlpcEndpoint *endpoint = static_cast<FakeAlpcEndpoint *>(
        fixture.client.portHandle);
    ALPC_PORT_MESSAGE closed = {};

    fixture.callbacks.asyncStatus = STATUS_PORT_DISCONNECTED;
    REQUIRE(AlpcPort_SendMessage(&fixture.client, message,
                                 static_cast<ULONG>(sizeof(message)), 0x41U,
                                 ALPC_PORT_SEND_FLAG_ASYNC, nullptr, nullptr,
                                 nullptr) == STATUS_SUCCESS);
    REQUIRE(AlpcPort_ProcessBlockedEvent(&fixture.server) ==
            STATUS_PORT_DISCONNECTED);
    REQUIRE(fixture.server.clientCount == 1U);
    REQUIRE(fixture.callbacks.closeCalls == 0U);

    fixture.callbacks.asyncStatus = STATUS_SUCCESS;
    REQUIRE(AlpcPort_SendMessage(&fixture.client, message,
                                 static_cast<ULONG>(sizeof(message)), 0x42U,
                                 ALPC_PORT_SEND_FLAG_ASYNC, nullptr, nullptr,
                                 nullptr) == STATUS_SUCCESS);
    REQUIRE(AlpcPort_ProcessBlockedEvent(&fixture.server) == STATUS_SUCCESS);
    REQUIRE(fixture.callbacks.asyncCalls == 2U);

    closed.u1.s1.TotalLength =
        static_cast<USHORT>(sizeof(ALPC_PORT_MESSAGE));
    closed.u1.s1.DataLength = 0U;
    closed.u2.s2.Type =
        static_cast<USHORT>(ALPC_PORT_MESSAGE_TYPE_PORT_CLOSED);
    enqueue_alpc_message(make_alpc_message(&closed, endpoint, nullptr, nullptr));
    REQUIRE(AlpcPort_ProcessBlockedEvent(&fixture.server) ==
            STATUS_PORT_DISCONNECTED);
    REQUIRE(fixture.server.clientCount == 0U);
    REQUIRE(fixture.callbacks.closeCalls == 1U);
}

TEST_CASE("ALPC deferred reply is one-shot and preserves control id",
          "[alpc][deferred]")
{
    AlpcFixture fixture = {};
    DeferredReplyState deferred = {};
    ReplyCapture capture = {};
    const unsigned char request[] = {'q'};
    const unsigned char response[] = {'o', 'k'};
    const std::vector<unsigned char> expectedResponse = {'o', 'k'};

    deferred.server = &fixture.server;
    g_deferred_reply = &deferred;
    fixture.events.onSyncRequest = alpc_deferred_request;
    REQUIRE(AlpcPort_Register_ServerEvtCallback(&fixture.server,
                                                 &fixture.events) != 0U);

    std::thread replyWorker([&deferred, &response] {
        ALPC_PORT_REPLY_TOKEN first = {};
        ALPC_PORT_REPLY_TOKEN duplicate = {};
        {
            std::unique_lock<std::mutex> lock(deferred.mutex);
            deferred.condition.wait(lock, [&deferred] {
                return deferred.captured;
            });
            first = deferred.token;
            duplicate = deferred.token;
        }
        deferred.replyStatus = AlpcPort_ServerReply(
            deferred.server, &first, response,
            static_cast<ULONG>(sizeof(response)), STATUS_SUCCESS, nullptr);
        deferred.duplicateStatus = AlpcPort_ServerReply(
            deferred.server, &duplicate, response,
            static_cast<ULONG>(sizeof(response)), STATUS_SUCCESS, nullptr);
    });

    REQUIRE(AlpcPort_SendMessage(&fixture.client, request,
                                 static_cast<ULONG>(sizeof(request)), 0x55U,
                                 0U, alpc_reply_capture, &capture, nullptr) ==
            STATUS_SUCCESS);
    replyWorker.join();
    REQUIRE(deferred.captureStatus == STATUS_SUCCESS);
    REQUIRE(deferred.replyStatus == STATUS_SUCCESS);
    REQUIRE(deferred.duplicateStatus == STATUS_INVALID_PARAMETER);
    REQUIRE(capture.bytes == expectedResponse);
    REQUIRE(g_alpc.lastReplyControlId == 0x55U);
    REQUIRE(List_Count(&fixture.server.pendingReplies) == 0U);
    REQUIRE(g_alpc.replyWithReceiveCalls == 0U);
    g_deferred_reply = static_cast<DeferredReplyState *>(0);
}

TEST_CASE("ALPC rejects a failed connection-complete wait",
          "[alpc][connect][validation]")
{
    ALPC_PORT_SERVER_CONTEXT server = {};
    ALPC_PORT_CLIENT_CONTEXT client = {};
    ALPC_PORT_SERVER_CONFIG serverConfig = {};
    ALPC_PORT_CLIENT_CONFIG clientConfig = {};
    ALPC_PORT_SERVER_EVENTS events = {};
    AlpcCallbackState callbacks = {};

    alpc_fill_name(&serverConfig.portName);
    serverConfig.maxMessageLength = kAlpcCapacity;
    serverConfig.maxClients = 1U;
    serverConfig.api = fake_alpc_server_apis();
    alpc_fill_name(&clientConfig.portName);
    clientConfig.maxMessageLength = kAlpcCapacity;
    clientConfig.api = fake_alpc_client_apis();
    events.onPreConnect = alpc_pre_connect;
    events.onPostConnect = alpc_post_connect;
    g_alpc_callbacks = &callbacks;

    REQUIRE(AlpcPort_ServerCreate(&serverConfig, &server) == STATUS_SUCCESS);
    g_alpc.serverContext = &server;
    REQUIRE(AlpcPort_Register_ServerEvtCallback(&server, &events) != 0U);
    g_alpc.emptyEndpointPollStatus = STATUS_PORT_DISCONNECTED;
    REQUIRE(AlpcPort_Connect(&clientConfig, &client) ==
            STATUS_PORT_DISCONNECTED);
    REQUIRE(client.initialized == 0);
    REQUIRE(client.portHandle == static_cast<HANDLE>(0));
    REQUIRE(client.sendLock.initialized == 0U);
    REQUIRE(client.receiveLock.initialized == 0U);
    REQUIRE(client.lifetime.initialized == 0U);

    AlpcPort_ServerClose(&server);
    g_alpc_callbacks = static_cast<AlpcCallbackState *>(0);
    g_alpc.reset();
}

TEST_CASE("ALPC disconnect removes a client exactly once", "[alpc][lifetime]")
{
    AlpcFixture fixture = {};
    HANDLE clientPort = fixture.client.portHandle;

    REQUIRE(AlpcPort_ServerDisconnectClient(&fixture.server, clientPort) ==
            STATUS_SUCCESS);
    REQUIRE(fixture.callbacks.closeCalls == 1U);
    REQUIRE(AlpcPort_ServerDisconnectClient(&fixture.server, clientPort) ==
            STATUS_INVALID_PARAMETER);
    REQUIRE(AlpcPort_SendMessage(&fixture.client, static_cast<const void *>(0), 0U,
                                 0U, ALPC_PORT_SEND_FLAG_ASYNC,
                                 static_cast<AlpcPort_SyncReplyCallback>(0),
                                 static_cast<PVOID>(0), nullptr) ==
            STATUS_PORT_DISCONNECTED);
}

TEST_CASE("ALPC endpoint tokens require both record identity and cookie",
          "[alpc][lifetime][token]")
{
    AlpcFixture fixture = {};
    ALPC_PORT_CLIENT_CONTEXT secondClient = {};
    ALPC_PORT_ENDPOINT_TOKEN firstToken = {};
    ALPC_PORT_ENDPOINT_TOKEN secondToken = {};
    ALPC_PORT_ENDPOINT_TOKEN forgedStaleToken = {};
    const unsigned char message[] = {'s', 'e', 'c', 'o', 'n', 'd'};

    REQUIRE(AlpcPort_ServerGetClientToken(
                &fixture.server, fixture.client.portHandle, &firstToken) ==
            STATUS_SUCCESS);
    REQUIRE(firstToken.opaque != nullptr);
    REQUIRE(firstToken.cookie != 0U);

    REQUIRE(AlpcPort_Connect(&fixture.clientConfig, &secondClient) ==
            STATUS_SUCCESS);
    REQUIRE(AlpcPort_ServerGetClientToken(
                &fixture.server, secondClient.portHandle, &secondToken) ==
            STATUS_SUCCESS);
    REQUIRE(secondToken.opaque != nullptr);
    REQUIRE(secondToken.cookie != 0U);
    REQUIRE(secondToken.cookie != firstToken.cookie);

    REQUIRE(AlpcPort_ServerDisconnectClientToken(&fixture.server,
                                                  &firstToken) ==
            STATUS_SUCCESS);
    REQUIRE(fixture.callbacks.closeCalls == 1U);
    REQUIRE(AlpcPort_ServerDisconnectClientToken(&fixture.server,
                                                  &firstToken) ==
            STATUS_PORT_DISCONNECTED);

    /* Model allocator-address or HANDLE reuse explicitly: even if a stale
       caller presents the current record pointer, its old cookie must not
       authorize disconnecting the replacement endpoint. */
    forgedStaleToken.opaque = secondToken.opaque;
    forgedStaleToken.cookie = firstToken.cookie;
    REQUIRE(AlpcPort_ServerDisconnectClientToken(&fixture.server,
                                                  &forgedStaleToken) ==
            STATUS_PORT_DISCONNECTED);
    REQUIRE(fixture.server.clientCount == 1U);
    REQUIRE(fixture.callbacks.closeCalls == 1U);

    REQUIRE(AlpcPort_SendMessage(
                &secondClient, message, static_cast<ULONG>(sizeof(message)),
                0x61U, ALPC_PORT_SEND_FLAG_ASYNC,
                static_cast<AlpcPort_SyncReplyCallback>(0),
                static_cast<PVOID>(0), nullptr) == STATUS_SUCCESS);
    REQUIRE(AlpcPort_ProcessBlockedEvent(&fixture.server) == STATUS_SUCCESS);
    REQUIRE(fixture.callbacks.asyncCalls == 1U);

    REQUIRE(AlpcPort_ServerDisconnectClientToken(&fixture.server,
                                                  &secondToken) ==
            STATUS_SUCCESS);
    REQUIRE(fixture.server.clientCount == 0U);
    REQUIRE(fixture.callbacks.closeCalls == 2U);
    AlpcPort_Disconnect(&secondClient);
}

TEST_CASE("ALPC dedicated receive lock does not block sends",
          "[alpc][transport][concurrency]")
{
    AlpcFixture fixture = {};
    const unsigned char message[] = {'f', 'u', 'l', 'l', '-', 'd', 'u', 'p', 'l', 'e', 'x'};
    std::mutex completionMutex;
    std::condition_variable completionCondition;
    NTSTATUS sendStatus = STATUS_UNSUCCESSFUL;
    bool sendCompleted = false;

    REQUIRE(AlpcPort_ClientAcquire(&fixture.client) != 0U);
    AlpcPort_ClientReceiveLock(&fixture.client);
    std::thread sender([&] {
        sendStatus = AlpcPort_SendMessage(
            &fixture.client, message, static_cast<ULONG>(sizeof(message)),
            0x77U, ALPC_PORT_SEND_FLAG_ASYNC,
            static_cast<AlpcPort_SyncReplyCallback>(0),
            static_cast<PVOID>(0), nullptr);
        {
            std::lock_guard<std::mutex> guard(completionMutex);
            sendCompleted = true;
        }
        completionCondition.notify_one();
    });

    bool completedWhileReceiveLocked = false;
    {
        std::unique_lock<std::mutex> lock(completionMutex);
        completedWhileReceiveLocked = completionCondition.wait_for(
            lock, std::chrono::seconds(2), [&] { return sendCompleted; });
    }
    AlpcPort_ClientReceiveUnlock(&fixture.client);
    AlpcPort_ClientRelease(&fixture.client);
    sender.join();

    REQUIRE(completedWhileReceiveLocked);
    REQUIRE(sendStatus == STATUS_SUCCESS);
    REQUIRE(AlpcPort_ProcessBlockedEvent(&fixture.server) == STATUS_SUCCESS);
    REQUIRE(fixture.callbacks.asyncCalls == 1U);
}
