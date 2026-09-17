/*
 * Catch2 regression tests for the user-mode LPC SDK.
 *
 * The tests use a small in-process Native API double.  It models the parts of
 * LPC that the SDK consumes (connection hand-off, request/reply, datagrams,
 * section-backed payloads and timed receive) without requiring a Windows
 * kernel or a second process.  Build lpc_port.c as C and link this file as C++:
 *
 *   gcc -std=c11 -I. -c lpc_port.c allocator.c libc.c list.c
 *   g++ -std=c++17 -I. lpc_unit_test.cpp lpc_port.o allocator.o libc.o list.o \
 *       -pthread -o lpc_unit_test.exe
 */

#define CATCH_CONFIG_MAIN
#include "catch2/catch.hpp"

#include "lpc_port.h"

/* MinGW's winternl headers sometimes expose status literals as unsigned
 * expressions.  Normalize them here so the strict C++ test build does not
 * diagnose an implementation-defined signed conversion. */
#undef STATUS_SUCCESS
#undef STATUS_UNSUCCESSFUL
#undef STATUS_INVALID_PARAMETER
#undef STATUS_INFO_LENGTH_MISMATCH
#undef STATUS_DATA_ERROR
#undef STATUS_INSUFFICIENT_RESOURCES
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

struct FakeLpcServer
{
    bool closed;

    FakeLpcServer() : closed(false)
    {
    }
};

struct FakeLpcSection
{
    std::vector<unsigned char> bytes;
    bool closed;

    FakeLpcSection() : bytes(), closed(false)
    {
    }
};

struct FakeLpcEndpoint
{
    FakeLpcSection *section;
    PVOID serverContext;
    bool closed;

    FakeLpcEndpoint()
        : section(static_cast<FakeLpcSection *>(0)),
          serverContext(static_cast<PVOID>(0)), closed(false)
    {
    }
};

struct FakeLpcConnect
{
    FakeLpcSection *section;
    FakeLpcEndpoint *endpoint;
    uint32_t responseControlId;
    bool accepted;
    bool denied;

    FakeLpcConnect()
        : section(static_cast<FakeLpcSection *>(0)),
          endpoint(static_cast<FakeLpcEndpoint *>(0)), responseControlId(0U),
          accepted(false), denied(false)
    {
    }
};

struct FakeLpcSync
{
    std::mutex mutex;
    std::condition_variable condition;
    alignas(8) std::array<unsigned char, LPC_PORT_BUFFER_SIZE> reply;
    std::size_t replyLength;
    NTSTATUS status;
    bool replied;

    FakeLpcSync()
        : mutex(), condition(), reply(), replyLength(0U),
          status(STATUS_PORT_DISCONNECTED), replied(false)
    {
    }
};

struct FakeLpcQueued
{
    alignas(8) std::array<unsigned char, LPC_PORT_BUFFER_SIZE> bytes;
    std::size_t length;
    FakeLpcEndpoint *endpoint;
    FakeLpcConnect *connect;
    FakeLpcSync *sync;

    FakeLpcQueued()
        : bytes(), length(0U), endpoint(static_cast<FakeLpcEndpoint *>(0)),
          connect(static_cast<FakeLpcConnect *>(0)),
          sync(static_cast<FakeLpcSync *>(0))
    {
    }
};

struct FakeLpcState
{
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<FakeLpcQueued> queue;
    FakeLpcServer *server;
    LPC_SERVER_CONTEXT *serverContext;
    FakeLpcQueued current;
    bool hasCurrent;
    bool mutateNextRequest;
    bool wow64Process;
    bool overflowClientView;
    bool overflowClientViewSize;
    bool highClientRemoteBase;
    bool overflowRemoteView;
    bool overflowRemoteViewSize;
    PVOID portContextOverride;
    ULONG peerMaxMessageLength;
    ULONG lastSyncRequestOptions;
    USHORT lastSyncRequestLength;
    ULONG lastClientViewLength;
    ULONG lastRemoteViewLength;
    uint32_t queryProcessCalls;
    std::vector<FakeLpcServer *> servers;
    std::vector<FakeLpcSection *> sections;
    std::vector<FakeLpcEndpoint *> endpoints;

    FakeLpcState()
        : mutex(), condition(), queue(), server(static_cast<FakeLpcServer *>(0)),
          serverContext(static_cast<LPC_SERVER_CONTEXT *>(0)), current(),
          hasCurrent(false), mutateNextRequest(false),
          wow64Process(sizeof(void *) == 4U), overflowClientView(false),
          overflowClientViewSize(false), highClientRemoteBase(false),
          overflowRemoteView(false), overflowRemoteViewSize(false),
          portContextOverride(static_cast<PVOID>(0)),
          peerMaxMessageLength(static_cast<ULONG>(LPC_PORT_NATIVE_MAX_MESSAGE_LENGTH)),
          lastSyncRequestOptions(0U), lastSyncRequestLength(0U),
          lastClientViewLength(0U), lastRemoteViewLength(0U),
          queryProcessCalls(0U), servers(), sections(), endpoints()
    {
    }

    void reset()
    {
        std::lock_guard<std::mutex> guard(mutex);
        queue.clear();
        server = static_cast<FakeLpcServer *>(0);
        serverContext = static_cast<LPC_SERVER_CONTEXT *>(0);
        current = FakeLpcQueued();
        hasCurrent = false;
        mutateNextRequest = false;
        wow64Process = sizeof(void *) == 4U;
        overflowClientView = false;
        overflowClientViewSize = false;
        highClientRemoteBase = false;
        overflowRemoteView = false;
        overflowRemoteViewSize = false;
        portContextOverride = static_cast<PVOID>(0);
        peerMaxMessageLength = static_cast<ULONG>(LPC_PORT_NATIVE_MAX_MESSAGE_LENGTH);
        lastSyncRequestOptions = 0U;
        lastSyncRequestLength = 0U;
        lastClientViewLength = 0U;
        lastRemoteViewLength = 0U;
        queryProcessCalls = 0U;
        for (FakeLpcEndpoint *endpoint : endpoints) {
            delete endpoint;
        }
        for (FakeLpcSection *section : sections) {
            delete section;
        }
        for (FakeLpcServer *item : servers) {
            delete item;
        }
        endpoints.clear();
        sections.clear();
        servers.clear();
    }
};

static FakeLpcState g_lpc = {};
static thread_local FakeLpcQueued g_lpc_thread_current = {};

static bool fake_lpc_uses_64bit_wire(void)
{
    return sizeof(void *) == 8U || g_lpc.wow64Process;
}

static std::size_t fake_lpc_native_header_size(void)
{
    return fake_lpc_uses_64bit_wire() ? sizeof(LPC_PORT_MESSAGE64)
                                      : sizeof(LPC_PORT_MESSAGE);
}

static USHORT fake_lpc_total_length(const void *message)
{
    if (fake_lpc_uses_64bit_wire()) {
        return static_cast<const LPC_PORT_MESSAGE64 *>(message)->u1.s1.TotalLength;
    }
    return static_cast<const LPC_PORT_MESSAGE *>(message)->u1.s1.TotalLength;
}

static void fake_lpc_set_message_header(void *message, USHORT dataLength,
                                        USHORT totalLength, USHORT type)
{
    if (fake_lpc_uses_64bit_wire()) {
        LPC_PORT_MESSAGE64 *native = static_cast<LPC_PORT_MESSAGE64 *>(message);
        native->u1.s1.DataLength = dataLength;
        native->u1.s1.TotalLength = totalLength;
        native->u2.s2.Type = type;
    } else {
        LPC_PORT_MESSAGE *native = static_cast<LPC_PORT_MESSAGE *>(message);
        native->u1.s1.DataLength = dataLength;
        native->u1.s1.TotalLength = totalLength;
        native->u2.s2.Type = type;
    }
}

static ULONG fake_lpc_control_id(const void *message)
{
    const unsigned char *bytes = static_cast<const unsigned char *>(message);
    return *reinterpret_cast<const ULONG *>(
        bytes + fake_lpc_native_header_size());
}

static ULONG fake_lpc_options(const void *message)
{
    const unsigned char *bytes = static_cast<const unsigned char *>(message);
    return *reinterpret_cast<const ULONG *>(
        bytes + fake_lpc_native_header_size() + sizeof(ULONG));
}

static void fake_lpc_set_control_id(void *message, ULONG value)
{
    unsigned char *bytes = static_cast<unsigned char *>(message);
    *reinterpret_cast<ULONG *>(bytes + fake_lpc_native_header_size()) = value;
}

static void fake_lpc_set_received_type(void *message, USHORT type)
{
    const USHORT nativeType = static_cast<USHORT>(
        type | (g_lpc.wow64Process ? 0x3000U : 0U));
    if (fake_lpc_uses_64bit_wire()) {
        static_cast<LPC_PORT_MESSAGE64 *>(message)->u2.s2.Type = nativeType;
    } else {
        static_cast<LPC_PORT_MESSAGE *>(message)->u2.s2.Type = nativeType;
    }
}

static FakeLpcQueued make_lpc_message(const PLPC_PORT_MESSAGE message,
                                      FakeLpcEndpoint *endpoint,
                                      FakeLpcConnect *connect,
                                      FakeLpcSync *sync)
{
    FakeLpcQueued queued = {};
    std::size_t length = fake_lpc_native_header_size();

    queued.endpoint = endpoint;
    queued.connect = connect;
    queued.sync = sync;
    if (message != static_cast<PLPC_PORT_MESSAGE>(0)) {
        length = fake_lpc_total_length(message);
        if (length < fake_lpc_native_header_size() ||
            length > LPC_PORT_BUFFER_SIZE) {
            length = fake_lpc_native_header_size();
        }
        std::memcpy(queued.bytes.data(), message, length);
    }
    queued.length = length;
    return queued;
}

static void enqueue_lpc_message(const FakeLpcQueued &queued)
{
    {
        std::lock_guard<std::mutex> guard(g_lpc.mutex);
        g_lpc.queue.push_back(queued);
    }
    g_lpc.condition.notify_all();
}

static bool fake_lpc_wait_for_message(const LARGE_INTEGER *timeout)
{
    std::unique_lock<std::mutex> lock(g_lpc.mutex);

    if (!g_lpc.queue.empty()) {
        return true;
    }
    if (timeout == static_cast<const LARGE_INTEGER *>(0)) {
        g_lpc.condition.wait(lock, [] { return !g_lpc.queue.empty(); });
        return true;
    }
    if (timeout->QuadPart == 0) {
        return false;
    }
    if (timeout->QuadPart < 0) {
        const std::int64_t ticks = -timeout->QuadPart;
        const std::int64_t milliseconds = ticks / 10000;
        const std::int64_t bounded = milliseconds > 60000 ? 60000 : milliseconds;
        return g_lpc.condition.wait_for(
            lock, std::chrono::milliseconds(static_cast<long long>(bounded)),
            [] { return !g_lpc.queue.empty(); });
    }
    /* Positive NT times are absolute system times.  A test double does not
       need a wall-clock conversion; treating an expired/unknown value as a
       poll keeps the result deterministic. */
    return false;
}

static NTSTATUS NTAPI fake_lpc_nt_close(HANDLE handle)
{
    FakeLpcServer *server = static_cast<FakeLpcServer *>(handle);

    if (handle == static_cast<HANDLE>(0)) {
        return STATUS_INVALID_PARAMETER;
    }
    for (FakeLpcServer *item : g_lpc.servers) {
        if (item == server) {
            item->closed = true;
            return STATUS_SUCCESS;
        }
    }
    for (FakeLpcEndpoint *endpoint : g_lpc.endpoints) {
        if (endpoint == handle) {
            endpoint->closed = true;
            return STATUS_SUCCESS;
        }
    }
    for (FakeLpcSection *section : g_lpc.sections) {
        if (section == handle) {
            section->closed = true;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_INVALID_PARAMETER;
}

static void NTAPI fake_lpc_init_ansi(PANSI_STRING destination, PCSZ source)
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

static NTSTATUS NTAPI fake_lpc_ansi_to_unicode(PUNICODE_STRING destination,
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

static void NTAPI fake_lpc_free_unicode(PUNICODE_STRING string)
{
    if (string != static_cast<PUNICODE_STRING>(0)) {
        delete[] string->Buffer;
        string->Buffer = static_cast<wchar_t *>(0);
        string->Length = 0;
        string->MaximumLength = 0;
    }
}

static NTSTATUS NTAPI fake_lpc_create_section(PHANDLE sectionHandle,
                                               ACCESS_MASK,
                                               POBJECT_ATTRIBUTES,
                                               LARGE_INTEGER *maximumSize,
                                               ULONG, ULONG, HANDLE)
{
    FakeLpcSection *section = new FakeLpcSection();
    std::size_t size = 0x1000U;

    if (sectionHandle == static_cast<PHANDLE>(0) ||
        maximumSize == static_cast<LARGE_INTEGER *>(0)) {
        delete section;
        return STATUS_INVALID_PARAMETER;
    }
    if (maximumSize->QuadPart > 0 && maximumSize->QuadPart < 0x4000000LL) {
        size = static_cast<std::size_t>(maximumSize->QuadPart);
    }
    section->bytes.resize(size);
    g_lpc.sections.push_back(section);
    *sectionHandle = static_cast<HANDLE>(section);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI fake_lpc_create_port(PHANDLE portHandle,
                                            POBJECT_ATTRIBUTES,
                                            ULONG, ULONG, ULONG)
{
    FakeLpcServer *server = new FakeLpcServer();

    if (portHandle == static_cast<PHANDLE>(0)) {
        delete server;
        return STATUS_INVALID_PARAMETER;
    }
    g_lpc.servers.push_back(server);
    g_lpc.server = server;
    *portHandle = static_cast<HANDLE>(server);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI fake_lpc_query_information_process(
    HANDLE, ULONG informationClass, PVOID information,
    ULONG informationLength, PULONG returnLength)
{
    ULONG_PTR *wow64Peb = static_cast<ULONG_PTR *>(information);

    ++g_lpc.queryProcessCalls;
    if (informationClass != 26U || wow64Peb == nullptr ||
        informationLength != static_cast<ULONG>(sizeof(*wow64Peb))) {
        return STATUS_INVALID_PARAMETER;
    }
    *wow64Peb = g_lpc.wow64Process ? static_cast<ULONG_PTR>(1U)
                                    : static_cast<ULONG_PTR>(0U);
    if (returnLength != nullptr) {
        *returnLength = static_cast<ULONG>(sizeof(*wow64Peb));
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI fake_lpc_accept_connect(PHANDLE portHandle,
                                               PVOID portContext,
                                               PLPC_PORT_MESSAGE connectionRequest,
                                               BOOLEAN acceptConnection,
                                               PLPC_PORT_VIEW,
                                               PLPC_REMOTE_PORT_VIEW clientView)
{
    FakeLpcConnect *connect = g_lpc.current.connect;

    if (connect == static_cast<FakeLpcConnect *>(0) ||
        connectionRequest == static_cast<PLPC_PORT_MESSAGE>(0)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!acceptConnection) {
        connect->denied = true;
        return STATUS_SUCCESS;
    }
    FakeLpcEndpoint *endpoint = new FakeLpcEndpoint();
    endpoint->section = connect->section;
    endpoint->serverContext = portContext;
    g_lpc.endpoints.push_back(endpoint);
    connect->endpoint = endpoint;
    connect->responseControlId = fake_lpc_control_id(connectionRequest);
    connect->accepted = true;
    if (portHandle != static_cast<PHANDLE>(0)) {
        *portHandle = static_cast<HANDLE>(endpoint);
    }
    if (clientView != static_cast<PLPC_REMOTE_PORT_VIEW>(0)) {
        if (fake_lpc_uses_64bit_wire()) {
            LPC_REMOTE_PORT_VIEW64 *wide =
                reinterpret_cast<LPC_REMOTE_PORT_VIEW64 *>(clientView);
            g_lpc.lastRemoteViewLength = wide->Length;
            wide->ViewSize = connect->section == nullptr
                                 ? 0U
                                 : static_cast<uint64_t>(
                                       connect->section->bytes.size());
            wide->ViewBase = connect->section == nullptr
                                 ? 0U
                                 : static_cast<uint64_t>(
                                       reinterpret_cast<std::uintptr_t>(
                                           connect->section->bytes.data()));
            if (g_lpc.overflowRemoteView && sizeof(void *) == 4U) {
                wide->ViewBase = (std::numeric_limits<uint64_t>::max)();
            }
            if (g_lpc.overflowRemoteViewSize && sizeof(void *) == 4U) {
                wide->ViewSize = (std::numeric_limits<uint64_t>::max)();
            }
        } else {
            g_lpc.lastRemoteViewLength = clientView->Length;
            clientView->ViewSize = connect->section == nullptr
                                       ? 0U
                                       : connect->section->bytes.size();
            clientView->ViewBase = connect->section == nullptr
                                       ? static_cast<PVOID>(0)
                                       : static_cast<PVOID>(
                                             connect->section->bytes.data());
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI fake_lpc_complete_connect(HANDLE)
{
    return STATUS_SUCCESS;
}

static NTSTATUS fake_lpc_process_connection(void)
{
    if (g_lpc.serverContext == static_cast<LPC_SERVER_CONTEXT *>(0)) {
        return STATUS_PORT_DISCONNECTED;
    }
    return LpcPort_ProcessBlockedEvent(g_lpc.serverContext);
}

static NTSTATUS NTAPI fake_lpc_connect(PHANDLE portHandle,
                                       PUNICODE_STRING,
                                       PSECURITY_QUALITY_OF_SERVICE,
                                       PLPC_PORT_VIEW clientView,
                                       PLPC_REMOTE_PORT_VIEW,
                                       PULONG maxMessageLength,
                                       PVOID connectionInformation,
                                       PULONG connectionInformationLength)
{
    FakeLpcConnect connect = {};
    uint32_t helloId = 0U;
    alignas(8) std::array<unsigned char, LPC_PORT_BUFFER_SIZE> connectionFrame = {};
    NTSTATUS processStatus = STATUS_UNSUCCESSFUL;

    if (portHandle == static_cast<PHANDLE>(0) || g_lpc.server == static_cast<FakeLpcServer *>(0) ||
        g_lpc.server->closed || clientView == static_cast<PLPC_PORT_VIEW>(0) ||
        connectionInformation == static_cast<PVOID>(0) ||
        connectionInformationLength == static_cast<PULONG>(0)) {
        return STATUS_PORT_DISCONNECTED;
    }
    if (fake_lpc_uses_64bit_wire()) {
        LPC_PORT_VIEW64 *wide = reinterpret_cast<LPC_PORT_VIEW64 *>(clientView);
        g_lpc.lastClientViewLength = wide->Length;
        connect.section = reinterpret_cast<FakeLpcSection *>(
            static_cast<std::uintptr_t>(wide->SectionHandle));
        wide->ViewBase = connect.section == nullptr
                             ? 0U
                             : static_cast<uint64_t>(
                                   reinterpret_cast<std::uintptr_t>(
                                       connect.section->bytes.data()));
        wide->ViewRemoteBase =
            g_lpc.highClientRemoteBase && sizeof(void *) == 4U
                ? (std::numeric_limits<uint64_t>::max)()
                : 0U;
        if (g_lpc.overflowClientView && sizeof(void *) == 4U) {
            wide->ViewBase = (std::numeric_limits<uint64_t>::max)();
        }
        if (g_lpc.overflowClientViewSize && sizeof(void *) == 4U) {
            wide->ViewSize = (std::numeric_limits<uint64_t>::max)();
        }
    } else {
        g_lpc.lastClientViewLength = clientView->Length;
        connect.section = static_cast<FakeLpcSection *>(clientView->SectionHandle);
        if (connect.section != nullptr) {
            clientView->ViewBase = static_cast<PVOID>(
                connect.section->bytes.data());
        }
        clientView->ViewRemoteBase = static_cast<PVOID>(0);
    }
    if (connect.section == static_cast<FakeLpcSection *>(0) || connect.section->closed) {
        return STATUS_INVALID_PARAMETER;
    }
    if (*connectionInformationLength < static_cast<ULONG>(sizeof(uint32_t))) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }
    helloId = *static_cast<uint32_t *>(connectionInformation);
    fake_lpc_set_control_id(connectionFrame.data(), helloId);
    fake_lpc_set_message_header(
        connectionFrame.data(), static_cast<USHORT>(sizeof(uint32_t)),
        static_cast<USHORT>(fake_lpc_native_header_size() + sizeof(uint32_t)),
        static_cast<USHORT>(LPC_TYPE_CONNECTION_REQUEST |
                            (g_lpc.wow64Process ? 0x3000U : 0U)));
    enqueue_lpc_message(make_lpc_message(
                                          reinterpret_cast<PLPC_PORT_MESSAGE>(
                                              connectionFrame.data()),
                                          static_cast<FakeLpcEndpoint *>(0),
                                          &connect, static_cast<FakeLpcSync *>(0)));
    processStatus = fake_lpc_process_connection();
    if (!NT_SUCCESS(processStatus) || connect.denied || !connect.accepted ||
        connect.endpoint == static_cast<FakeLpcEndpoint *>(0)) {
        return NT_SUCCESS(processStatus) && connect.denied
                   ? STATUS_PORT_DISCONNECTED
                   : processStatus;
    }
    *portHandle = static_cast<HANDLE>(connect.endpoint);
    if (maxMessageLength != static_cast<PULONG>(0)) {
        *maxMessageLength = g_lpc.peerMaxMessageLength;
    }
    if (connectionInformationLength != static_cast<PULONG>(0)) {
        if (*connectionInformationLength < static_cast<ULONG>(sizeof(uint32_t))) {
            return STATUS_INFO_LENGTH_MISMATCH;
        }
        *connectionInformationLength = static_cast<ULONG>(sizeof(uint32_t));
        *static_cast<uint32_t *>(connectionInformation) = connect.responseControlId;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS fake_lpc_receive(PLPC_PORT_MESSAGE receiveMessage,
                                  PVOID *portContext,
                                  const LARGE_INTEGER *timeout)
{
    if (receiveMessage == static_cast<PLPC_PORT_MESSAGE>(0) ||
        !fake_lpc_wait_for_message(timeout)) {
        return timeout == static_cast<const LARGE_INTEGER *>(0)
                   ? STATUS_PORT_DISCONNECTED
                   : STATUS_TIMEOUT;
    }
    {
        std::lock_guard<std::mutex> guard(g_lpc.mutex);
        if (g_lpc.queue.empty()) {
            return STATUS_TIMEOUT;
        }
        g_lpc.current = g_lpc.queue.front();
        g_lpc.queue.pop_front();
        g_lpc.hasCurrent = true;
    }
    g_lpc_thread_current = g_lpc.current;
    std::memset(receiveMessage, 0, LPC_PORT_BUFFER_SIZE);
    std::memcpy(receiveMessage, g_lpc.current.bytes.data(), g_lpc.current.length);
    if (portContext != static_cast<PVOID *>(0)) {
        *portContext = g_lpc.portContextOverride != static_cast<PVOID>(0)
                           ? g_lpc.portContextOverride
                           : (g_lpc.current.endpoint ==
                                      static_cast<FakeLpcEndpoint *>(0)
                                  ? static_cast<PVOID>(0)
                                  : g_lpc.current.endpoint->serverContext);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI fake_lpc_reply_wait_receive(HANDLE,
                                                   PVOID *portContext,
                                                   PVOID,
                                                   PLPC_PORT_MESSAGE receiveMessage)
{
    return fake_lpc_receive(receiveMessage, portContext,
                             static_cast<const LARGE_INTEGER *>(0));
}

static NTSTATUS NTAPI fake_lpc_reply_wait_receive_ex(HANDLE,
                                                      PVOID *portContext,
                                                      PVOID,
                                                      PLPC_PORT_MESSAGE receiveMessage,
                                                      PLARGE_INTEGER timeout)
{
    return fake_lpc_receive(receiveMessage, portContext, timeout);
}

static NTSTATUS NTAPI fake_lpc_reply(HANDLE, PLPC_PORT_MESSAGE replyMessage)
{
    FakeLpcSync *sync = g_lpc_thread_current.sync;
    std::size_t length = fake_lpc_native_header_size();

    if (sync == static_cast<FakeLpcSync *>(0) ||
        replyMessage == static_cast<PLPC_PORT_MESSAGE>(0)) {
        return STATUS_INVALID_PARAMETER;
    }
    length = fake_lpc_total_length(replyMessage);
    if (length < fake_lpc_native_header_size() ||
        length > LPC_PORT_BUFFER_SIZE) {
        return STATUS_DATA_ERROR;
    }
    fake_lpc_set_received_type(replyMessage,
                               static_cast<USHORT>(LPC_TYPE_REPLY));
    {
        std::lock_guard<std::mutex> guard(sync->mutex);
        std::memcpy(sync->reply.data(), replyMessage, length);
        sync->replyLength = length;
        sync->status = STATUS_SUCCESS;
        sync->replied = true;
    }
    sync->condition.notify_all();
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI fake_lpc_request_port(HANDLE portHandle,
                                             PLPC_PORT_MESSAGE requestMessage)
{
    FakeLpcEndpoint *endpoint = static_cast<FakeLpcEndpoint *>(portHandle);

    if (endpoint == static_cast<FakeLpcEndpoint *>(0) || endpoint->closed ||
        requestMessage == static_cast<PLPC_PORT_MESSAGE>(0)) {
        return STATUS_PORT_DISCONNECTED;
    }
    FakeLpcQueued queued = make_lpc_message(
        requestMessage, endpoint, static_cast<FakeLpcConnect *>(0),
        static_cast<FakeLpcSync *>(0));
    fake_lpc_set_received_type(queued.bytes.data(),
                               static_cast<USHORT>(LPC_TYPE_DATAGRAM));
    enqueue_lpc_message(queued);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI fake_lpc_request_wait_reply(HANDLE portHandle,
                                                   PLPC_PORT_MESSAGE requestMessage,
                                                   PLPC_PORT_MESSAGE replyMessage)
{
    FakeLpcEndpoint *endpoint = static_cast<FakeLpcEndpoint *>(portHandle);
    FakeLpcSync sync = {};
    FakeLpcQueued queued = {};
    NTSTATUS processStatus = STATUS_UNSUCCESSFUL;

    if (endpoint == static_cast<FakeLpcEndpoint *>(0) || endpoint->closed ||
        requestMessage == static_cast<PLPC_PORT_MESSAGE>(0) ||
        replyMessage == static_cast<PLPC_PORT_MESSAGE>(0)) {
        return STATUS_PORT_DISCONNECTED;
    }
    g_lpc.lastSyncRequestOptions = fake_lpc_options(requestMessage);
    g_lpc.lastSyncRequestLength = fake_lpc_total_length(requestMessage);
    queued = make_lpc_message(requestMessage, endpoint,
                               static_cast<FakeLpcConnect *>(0), &sync);
    fake_lpc_set_received_type(queued.bytes.data(),
                               static_cast<USHORT>(LPC_TYPE_REQUEST));
    if (g_lpc.mutateNextRequest) {
        g_lpc.mutateNextRequest = false;
        fake_lpc_set_message_header(
            queued.bytes.data(), 0U,
            static_cast<USHORT>(fake_lpc_native_header_size() - 1U),
            static_cast<USHORT>(LPC_TYPE_REQUEST |
                                (g_lpc.wow64Process ? 0x3000U : 0U)));
    }
    enqueue_lpc_message(queued);
    processStatus = LpcPort_ProcessBlockedEvent(g_lpc.serverContext);
    if (!NT_SUCCESS(processStatus) && !sync.replied) {
        return processStatus;
    }
    {
        std::unique_lock<std::mutex> lock(sync.mutex);
        sync.condition.wait(lock, [&sync] { return sync.replied; });
    }
    std::memset(replyMessage, 0, LPC_PORT_BUFFER_SIZE);
    std::memcpy(replyMessage, sync.reply.data(), sync.replyLength);
    return sync.status;
}

static LPC_CLIENT_APIS fake_lpc_client_apis(void)
{
    LPC_CLIENT_APIS api = {};
    api.pfnRtlInitAnsiString = fake_lpc_init_ansi;
    api.pfnNtCreateSection = fake_lpc_create_section;
    api.pfnNtConnectPort = fake_lpc_connect;
    api.pfnNtClose = fake_lpc_nt_close;
    api.pfnRtlAnsiStringToUnicodeString = fake_lpc_ansi_to_unicode;
    api.pfnRtlFreeUnicodeString = fake_lpc_free_unicode;
    api.pfnNtRequestPort = fake_lpc_request_port;
    api.pfnNtRequestWaitReplyPort = fake_lpc_request_wait_reply;
    api.pfnNtQueryInformationProcess = fake_lpc_query_information_process;
    return api;
}

static LPC_SERVER_APIS fake_lpc_server_apis(void)
{
    LPC_SERVER_APIS api = {};
    api.pfnRtlInitAnsiString = fake_lpc_init_ansi;
    api.pfnNtCreatePort = fake_lpc_create_port;
    api.pfnNtClose = fake_lpc_nt_close;
    api.pfnRtlAnsiStringToUnicodeString = fake_lpc_ansi_to_unicode;
    api.pfnRtlFreeUnicodeString = fake_lpc_free_unicode;
    api.pfnNtReplyWaitReceivePort = fake_lpc_reply_wait_receive;
    api.pfnNtAcceptConnectPort = fake_lpc_accept_connect;
    api.pfnNtCompleteConnectPort = fake_lpc_complete_connect;
    api.pfnNtReplyPort = fake_lpc_reply;
    api.pfnNtReplyWaitReceivePortEx = fake_lpc_reply_wait_receive_ex;
    api.pfnNtQueryInformationProcess = fake_lpc_query_information_process;
    return api;
}

struct LpcCallbackState
{
    uint32_t preConnectCalls;
    uint32_t postConnectCalls;
    uint32_t syncCalls;
    uint32_t asyncCalls;
    uint32_t closeCalls;
    uint32_t lastControlId;
    std::vector<unsigned char> lastAsync;

    LpcCallbackState()
        : preConnectCalls(0U), postConnectCalls(0U), syncCalls(0U),
          asyncCalls(0U), closeCalls(0U), lastControlId(0U), lastAsync()
    {
    }
};

static LpcCallbackState *g_lpc_callbacks = static_cast<LpcCallbackState *>(0);

static void lpc_pre_connect(uint32_t *responseControlId, uint8_t *deny)
{
    if (g_lpc_callbacks != static_cast<LpcCallbackState *>(0)) {
        ++g_lpc_callbacks->preConnectCalls;
    }
    if (responseControlId != static_cast<uint32_t *>(0)) {
        *responseControlId += 0x10U;
    }
    if (deny != static_cast<uint8_t *>(0)) {
        *deny = 0U;
    }
}

static void lpc_post_connect(HANDLE, uint32_t controlId)
{
    if (g_lpc_callbacks != static_cast<LpcCallbackState *>(0)) {
        ++g_lpc_callbacks->postConnectCalls;
        g_lpc_callbacks->lastControlId = controlId;
    }
}

static void lpc_sync_request(HANDLE, uint32_t controlId, uint8_t *message,
                             uint32_t size, uint64_t)
{
    if (g_lpc_callbacks != static_cast<LpcCallbackState *>(0)) {
        ++g_lpc_callbacks->syncCalls;
        g_lpc_callbacks->lastControlId = controlId;
    }
    if (message != static_cast<uint8_t *>(0) && size != 0U) {
        message[0] ^= 0x20U;
        message[size - 1U] ^= 0x20U;
    }
}

static void lpc_async_request(HANDLE, uint32_t, uint8_t *message,
                              uint32_t size, uint64_t)
{
    if (g_lpc_callbacks != static_cast<LpcCallbackState *>(0)) {
        ++g_lpc_callbacks->asyncCalls;
        g_lpc_callbacks->lastAsync.assign(message, message + size);
    }
}

static void lpc_close(HANDLE)
{
    if (g_lpc_callbacks != static_cast<LpcCallbackState *>(0)) {
        ++g_lpc_callbacks->closeCalls;
    }
}

static void lpc_fill_name(LPC_PORT_NAME *name)
{
    const char value[] = "\\RPC Control\\UnitLpc";
    if (name != static_cast<LPC_PORT_NAME *>(0)) {
        std::memset(name, 0, sizeof(*name));
        std::memcpy(name->name, value, sizeof(value));
    }
}

struct LpcFixture
{
    LPC_SERVER_CONTEXT server;
    LPC_CLIENT_CONTEXT client;
    LPC_SERVER_CONFIG serverConfig;
    LPC_CLIENT_CONFIG clientConfig;
    LPC_SERVER_EVT_CONTEXT events;
    LpcCallbackState callbacks;
    uint32_t responseId;

    LpcFixture()
        : server(), client(), serverConfig(), clientConfig(), events(),
          callbacks(), responseId(0U)
    {
        lpc_fill_name(&serverConfig.LpcName);
        serverConfig.api = fake_lpc_server_apis();
        serverConfig.maxClients = 8U;
        lpc_fill_name(&clientConfig.LpcName);
        clientConfig.HelloId = 0x42U;
        clientConfig.lpRespID = &responseId;
        clientConfig.api = fake_lpc_client_apis();
        events.onPreConnect = lpc_pre_connect;
        events.onPostConnect = lpc_post_connect;
        events.onSyncRequest = lpc_sync_request;
        events.onAsyncRequest = lpc_async_request;
        events.onClose = lpc_close;
        g_lpc_callbacks = &callbacks;
        REQUIRE(LpcPort_ServerCreate(&serverConfig, &server) == STATUS_SUCCESS);
        g_lpc.serverContext = &server;
        REQUIRE(LpcPort_Register_ServerEvtCallback(&server, &events) != 0U);
        REQUIRE(LpcPort_Connect(&clientConfig, &client) == STATUS_SUCCESS);
    }

    ~LpcFixture()
    {
        LpcPort_Disconnect(&client);
        LpcPort_ServerClose(&server);
        g_lpc_callbacks = static_cast<LpcCallbackState *>(0);
        g_lpc.reset();
    }

    LpcFixture(const LpcFixture &) = delete;
    LpcFixture &operator=(const LpcFixture &) = delete;
};

struct ReplyCapture
{
    std::vector<unsigned char> bytes;
    uint32_t calls;

    ReplyCapture() : bytes(), calls(0U)
    {
    }
};

static NTSTATUS NTAPI lpc_reply_capture(PVOID buffer, ULONG length, PVOID context)
{
    ReplyCapture *capture = static_cast<ReplyCapture *>(context);

    if (capture == static_cast<ReplyCapture *>(0)) {
        return STATUS_INVALID_PARAMETER;
    }
    ++capture->calls;
    capture->bytes.clear();
    if (buffer != static_cast<PVOID>(0) && length != 0U) {
        const unsigned char *begin = static_cast<const unsigned char *>(buffer);
        capture->bytes.assign(begin, begin + length);
    }
    return STATUS_SUCCESS;
}

} /* namespace */

TEST_CASE("LPC validates configuration and preserves timed receive", "[lpc][validation]")
{
    LPC_SERVER_CONTEXT server = {};
    LPC_SERVER_CONFIG config = {};
    LPC_SERVER_APIS api = fake_lpc_server_apis();
    LARGE_INTEGER timeout = {};

    REQUIRE(LpcPort_ServerCreate(static_cast<PLPC_SERVER_CONFIG>(0), &server) ==
            STATUS_INVALID_PARAMETER);
    lpc_fill_name(&config.LpcName);
    config.api = api;
    REQUIRE(LpcPort_ServerCreate(&config, &server) == STATUS_SUCCESS);
    g_lpc.serverContext = &server;
    timeout.QuadPart = 0;
    REQUIRE(LpcPort_ProcessBlockedEventEx(&server, &timeout) == STATUS_TIMEOUT);
    timeout.QuadPart = -10000;
    REQUIRE(LpcPort_ProcessBlockedEventEx(&server, &timeout) == STATUS_TIMEOUT);
    LpcPort_ServerClose(&server);
    g_lpc.reset();
}

TEST_CASE("LPC selects the 64-bit Native wire layout when required",
          "[lpc][wow64][wire]")
{
    LpcFixture fixture = {};

    REQUIRE(sizeof(LPC_PORT_MESSAGE64) == 40U);
    REQUIRE(sizeof(LPC_PORT_VIEW64) == 48U);
    REQUIRE(sizeof(LPC_REMOTE_PORT_VIEW64) == 24U);
    REQUIRE(LPC_HEADER64_DATA_OFFSET == 48U);
    REQUIRE(fixture.server.use64BitWire == 1U);
    REQUIRE(fixture.client.use64BitWire == 1U);
    REQUIRE(g_lpc.lastClientViewLength == sizeof(LPC_PORT_VIEW64));
    REQUIRE(g_lpc.lastRemoteViewLength == sizeof(LPC_REMOTE_PORT_VIEW64));
#if defined(_WIN64) || defined(_M_AMD64) || defined(__x86_64__) || \
    defined(_M_ARM64) || defined(__aarch64__)
    REQUIRE(g_lpc.queryProcessCalls == 0U);
#else
    REQUIRE(g_lpc.queryProcessCalls == 2U);
#endif
}

TEST_CASE("LPC rejects an invalid negotiated message limit",
          "[lpc][validation][connect]")
{
    LPC_SERVER_CONTEXT server = {};
    LPC_CLIENT_CONTEXT client = {};
    LPC_SERVER_CONFIG serverConfig = {};
    LPC_CLIENT_CONFIG clientConfig = {};
    LPC_SERVER_EVT_CONTEXT events = {};
    LpcCallbackState callbacks = {};

    lpc_fill_name(&serverConfig.LpcName);
    serverConfig.api = fake_lpc_server_apis();
    lpc_fill_name(&clientConfig.LpcName);
    clientConfig.api = fake_lpc_client_apis();
    events.onPreConnect = lpc_pre_connect;
    events.onPostConnect = lpc_post_connect;
    g_lpc_callbacks = &callbacks;

    REQUIRE(LpcPort_ServerCreate(&serverConfig, &server) == STATUS_SUCCESS);
    g_lpc.serverContext = &server;
    REQUIRE(LpcPort_Register_ServerEvtCallback(&server, &events) != 0U);

    SECTION("zero is not treated as an unlimited peer")
    {
        g_lpc.peerMaxMessageLength = 0U;
        REQUIRE(LpcPort_Connect(&clientConfig, &client) == STATUS_DATA_ERROR);
        REQUIRE(client.initialized == 0);
    }

    SECTION("the limit must hold the inline envelope")
    {
        g_lpc.peerMaxMessageLength = static_cast<ULONG>(
            LPC_HEADER64_DATA_OFFSET + sizeof(ULONG) - 1U);
        REQUIRE(LpcPort_Connect(&clientConfig, &client) == STATUS_DATA_ERROR);
        REQUIRE(client.initialized == 0);
    }

    LpcPort_Disconnect(&client);
    LpcPort_ServerClose(&server);
    g_lpc_callbacks = static_cast<LpcCallbackState *>(0);
    g_lpc.reset();
}

#if !defined(_WIN64) && !defined(_M_AMD64) && !defined(__x86_64__) && \
    !defined(_M_ARM64) && !defined(__aarch64__)
TEST_CASE("LPC preserves the native 32-bit wire layout",
          "[lpc][x86][wire]")
{
    g_lpc.wow64Process = false;
    {
        LpcFixture fixture = {};
        ReplyCapture capture = {};
        const unsigned char message[] = {'x', '8', '6'};

        REQUIRE(fixture.server.use64BitWire == 0U);
        REQUIRE(fixture.client.use64BitWire == 0U);
        REQUIRE(g_lpc.lastClientViewLength == sizeof(LPC_PORT_VIEW));
        REQUIRE(g_lpc.lastRemoteViewLength == sizeof(LPC_REMOTE_PORT_VIEW));
        REQUIRE(LpcPort_SendMessage(
                    &fixture.client, message,
                    static_cast<ULONG>(sizeof(message)), 0x71U, 0U, 0U,
                    lpc_reply_capture, &capture) == STATUS_SUCCESS);
        REQUIRE(capture.calls == 1U);
        REQUIRE(capture.bytes.size() == sizeof(message));
    }
}

TEST_CASE("LPC requires runtime WOW64 detection on 32-bit user mode",
          "[lpc][wow64][validation]")
{
    LPC_SERVER_CONTEXT server = {};
    LPC_CLIENT_CONTEXT client = {};
    LPC_SERVER_CONFIG serverConfig = {};
    LPC_CLIENT_CONFIG clientConfig = {};

    lpc_fill_name(&serverConfig.LpcName);
    serverConfig.api = fake_lpc_server_apis();
    serverConfig.api.pfnNtQueryInformationProcess = nullptr;
    REQUIRE(LpcPort_ServerCreate(&serverConfig, &server) ==
            STATUS_INVALID_PARAMETER);

    lpc_fill_name(&clientConfig.LpcName);
    clientConfig.api = fake_lpc_client_apis();
    clientConfig.api.pfnNtQueryInformationProcess = nullptr;
    REQUIRE(LpcPort_Connect(&clientConfig, &client) ==
            STATUS_INVALID_PARAMETER);
    g_lpc.reset();
}

TEST_CASE("LPC validates WOW64 view conversions without truncating peer VA",
          "[lpc][wow64][view]")
{
    LPC_SERVER_CONTEXT server = {};
    LPC_CLIENT_CONTEXT client = {};
    LPC_SERVER_CONFIG serverConfig = {};
    LPC_CLIENT_CONFIG clientConfig = {};
    LPC_SERVER_EVT_CONTEXT events = {};
    LpcCallbackState callbacks = {};
    uint32_t responseId = 0U;

    lpc_fill_name(&serverConfig.LpcName);
    serverConfig.maxClients = 2U;
    serverConfig.api = fake_lpc_server_apis();
    lpc_fill_name(&clientConfig.LpcName);
    clientConfig.HelloId = 0x81U;
    clientConfig.lpRespID = &responseId;
    clientConfig.api = fake_lpc_client_apis();
    events.onPreConnect = lpc_pre_connect;
    events.onPostConnect = lpc_post_connect;
    events.onSyncRequest = lpc_sync_request;
    events.onAsyncRequest = lpc_async_request;
    events.onClose = lpc_close;
    g_lpc_callbacks = &callbacks;

    REQUIRE(LpcPort_ServerCreate(&serverConfig, &server) == STATUS_SUCCESS);
    g_lpc.serverContext = &server;
    REQUIRE(LpcPort_Register_ServerEvtCallback(&server, &events) != 0U);

    SECTION("a high peer ViewRemoteBase remains valid")
    {
        g_lpc.highClientRemoteBase = true;
        REQUIRE(LpcPort_Connect(&clientConfig, &client) == STATUS_SUCCESS);
        REQUIRE(client.client_view.ViewRemoteBase == nullptr);
    }

    SECTION("an unrepresentable local client ViewBase is rejected")
    {
        g_lpc.overflowClientView = true;
        REQUIRE(LpcPort_Connect(&clientConfig, &client) ==
                STATUS_INTEGER_OVERFLOW);
        REQUIRE(client.initialized == 0);
    }

    SECTION("an unrepresentable client ViewSize is rejected")
    {
        g_lpc.overflowClientViewSize = true;
        REQUIRE(LpcPort_Connect(&clientConfig, &client) ==
                STATUS_INTEGER_OVERFLOW);
        REQUIRE(client.initialized == 0);
    }

    SECTION("an unrepresentable server ViewBase is rejected")
    {
        g_lpc.overflowRemoteView = true;
        REQUIRE(LpcPort_Connect(&clientConfig, &client) ==
                STATUS_INTEGER_OVERFLOW);
        REQUIRE(server.clientCount == 0U);
    }

    SECTION("an unrepresentable server ViewSize is rejected")
    {
        g_lpc.overflowRemoteViewSize = true;
        REQUIRE(LpcPort_Connect(&clientConfig, &client) ==
                STATUS_INTEGER_OVERFLOW);
        REQUIRE(server.clientCount == 0U);
    }

    LpcPort_Disconnect(&client);
    LpcPort_ServerClose(&server);
    g_lpc_callbacks = static_cast<LpcCallbackState *>(0);
    g_lpc.reset();
}
#endif

TEST_CASE("LPC performs inline, shared-memory and asynchronous exchanges", "[lpc][transport]")
{
    LpcFixture fixture = {};
    ReplyCapture capture = {};
    const unsigned char inlineMessage[] = {'h', 'E', 'l', 'L', 'o'};
    const unsigned char asyncMessage[] = {'a', 's', 'y', 'n', 'c'};
    std::vector<unsigned char> largeMessage(LPC_MESSAGE_MAX_PACK_SIZE + 32U, 0x5AU);
    std::vector<unsigned char> expectedInline = {'H', 'E', 'l', 'L', 'O'};

    REQUIRE(LpcPort_SendMessage(&fixture.client, inlineMessage,
                                static_cast<ULONG>(sizeof(inlineMessage)),
                                7U, 0U, 0U, lpc_reply_capture, &capture) ==
            STATUS_SUCCESS);
    REQUIRE(capture.calls == 1U);
    REQUIRE(capture.bytes == expectedInline);
    REQUIRE(fixture.callbacks.syncCalls == 1U);
    REQUIRE(fixture.callbacks.lastControlId == 7U);

    REQUIRE(LpcPort_SendMessage(&fixture.client, asyncMessage,
                                static_cast<ULONG>(sizeof(asyncMessage)),
                                9U, 1U, 0U, static_cast<typedef_LpcSyncMsgReplyCallback>(0),
                                static_cast<void *>(0)) == STATUS_SUCCESS);
    REQUIRE(LpcPort_ProcessBlockedEvent(&fixture.server) == STATUS_SUCCESS);
    REQUIRE(fixture.callbacks.asyncCalls == 1U);
    REQUIRE(fixture.callbacks.lastAsync.size() == sizeof(asyncMessage));

    largeMessage.front() = 0x11U;
    largeMessage.back() = 0x22U;
    capture = ReplyCapture();
    REQUIRE(LpcPort_SendMessage(&fixture.client, largeMessage.data(),
                                static_cast<ULONG>(largeMessage.size()),
                                11U, 0U, 0U, lpc_reply_capture, &capture) ==
            STATUS_SUCCESS);
    REQUIRE(capture.calls == 1U);
    REQUIRE(capture.bytes.size() == largeMessage.size());
    REQUIRE(capture.bytes.front() == static_cast<unsigned char>(0x31U));
    REQUIRE(capture.bytes.back() == static_cast<unsigned char>(0x02U));
}

TEST_CASE("LPC honors the peer maximum message length",
          "[lpc][transport][cross-bitness]")
{
    g_lpc.peerMaxMessageLength = 328U;
    LpcFixture fixture = {};
    ReplyCapture capture = {};
    std::vector<unsigned char> boundary(276U, 0x31U);
    std::vector<unsigned char> overBoundary(277U, 0x32U);
    std::vector<unsigned char> crossBitnessPayload(300U, 0x33U);

    REQUIRE(fixture.client.negotiatedMaxMessageLength == 328U);

    REQUIRE(LpcPort_SendMessage(&fixture.client, boundary.data(),
                                static_cast<ULONG>(boundary.size()),
                                21U, 0U, 0U, lpc_reply_capture, &capture) ==
            STATUS_SUCCESS);
    REQUIRE(g_lpc.lastSyncRequestOptions == 0U);
    REQUIRE(g_lpc.lastSyncRequestLength == 328U);

    capture = ReplyCapture();
    REQUIRE(LpcPort_SendMessage(&fixture.client, overBoundary.data(),
                                static_cast<ULONG>(overBoundary.size()),
                                22U, 0U, 0U, lpc_reply_capture, &capture) ==
            STATUS_SUCCESS);
    REQUIRE((g_lpc.lastSyncRequestOptions & 1U) != 0U);
    REQUIRE(g_lpc.lastSyncRequestLength == LPC_HEADER64_DATA_OFFSET);

    capture = ReplyCapture();
    REQUIRE(LpcPort_SendMessage(&fixture.client, crossBitnessPayload.data(),
                                static_cast<ULONG>(crossBitnessPayload.size()),
                                23U, 0U, 0U, lpc_reply_capture, &capture) ==
            STATUS_SUCCESS);
    REQUIRE((g_lpc.lastSyncRequestOptions & 1U) != 0U);
    REQUIRE(g_lpc.lastSyncRequestLength == LPC_HEADER64_DATA_OFFSET);
}

TEST_CASE("LPC rejects oversized and malformed requests without stranding the sender",
          "[lpc][validation][malformed]")
{
    LpcFixture fixture = {};
    ReplyCapture capture = {};
    std::vector<unsigned char> oversized(LPC_MESSAGE_MAX_PACK_SIZE + 1U, 0xA5U);
    const unsigned char message[] = {'x', 'y'};

    REQUIRE(LpcPort_SendMessage(&fixture.client, oversized.data(),
                                static_cast<ULONG>(oversized.size()), 1U, 1U, 0U,
                                static_cast<typedef_LpcSyncMsgReplyCallback>(0),
                                static_cast<void *>(0)) == STATUS_NOT_SUPPORTED);
    g_lpc.mutateNextRequest = true;
    REQUIRE(LpcPort_SendMessage(&fixture.client, message,
                                static_cast<ULONG>(sizeof(message)), 2U, 0U, 0U,
                                lpc_reply_capture, &capture) == STATUS_SUCCESS);
    REQUIRE(capture.calls == 1U);
    REQUIRE(capture.bytes.empty());
    REQUIRE(fixture.callbacks.syncCalls == 0U);
}

TEST_CASE("LPC close and explicit client removal are idempotent", "[lpc][lifetime]")
{
    LpcFixture fixture = {};
    HANDLE clientHandle = fixture.client.hLPCPortHandle;

    REQUIRE(LpcPort_ServerDisconnectClient(&fixture.server, clientHandle) ==
            STATUS_SUCCESS);
    REQUIRE(fixture.callbacks.closeCalls == 1U);
    REQUIRE(LpcPort_ServerDisconnectClient(&fixture.server, clientHandle) ==
            STATUS_INVALID_PARAMETER);
    REQUIRE(LpcPort_SendMessage(&fixture.client, static_cast<const void *>(0), 0U,
                                0U, 1U, 0U,
                                static_cast<typedef_LpcSyncMsgReplyCallback>(0),
                                static_cast<void *>(0)) == STATUS_PORT_DISCONNECTED);
    LpcPort_ServerClose(&fixture.server);
    LpcPort_ServerClose(&fixture.server);
}

TEST_CASE("LPC PortContext tokens reject stale and HANDLE identities",
          "[lpc][routing][aba]")
{
    LpcFixture fixture = {};
    LPC_CLIENT_CONTEXT replacement = {};
    LPC_CLIENT_CONFIG replacementConfig = fixture.clientConfig;
    uint32_t replacementResponseId = 0U;
    const unsigned char message[] = {'n', 'e', 'w'};
    FakeLpcEndpoint *firstEndpoint =
        static_cast<FakeLpcEndpoint *>(fixture.client.hLPCPortHandle);
    PLPC_SERVER_CLIENT_INFO firstRecord =
        static_cast<PLPC_SERVER_CLIENT_INFO>(List_Head(&fixture.server.clientList));
    PVOID firstToken = firstEndpoint->serverContext;

    REQUIRE(firstToken != static_cast<PVOID>(0));
    REQUIRE(firstToken != static_cast<PVOID>(firstEndpoint));
    REQUIRE(firstToken != static_cast<PVOID>(firstRecord));
    REQUIRE(LpcPort_ServerDisconnectClient(&fixture.server,
                                            fixture.client.hLPCPortHandle) ==
            STATUS_SUCCESS);

    replacementConfig.lpRespID = &replacementResponseId;
    REQUIRE(LpcPort_Connect(&replacementConfig, &replacement) == STATUS_SUCCESS);
    FakeLpcEndpoint *replacementEndpoint =
        static_cast<FakeLpcEndpoint *>(replacement.hLPCPortHandle);
    PLPC_SERVER_CLIENT_INFO replacementRecord =
        static_cast<PLPC_SERVER_CLIENT_INFO>(List_Head(&fixture.server.clientList));
    const PVOID replacementToken = replacementEndpoint->serverContext;

    REQUIRE(replacementToken != static_cast<PVOID>(0));
    REQUIRE(replacementToken != firstToken);
    REQUIRE(replacementToken != static_cast<PVOID>(replacementEndpoint));
    REQUIRE(replacementToken != static_cast<PVOID>(replacementRecord));

    g_lpc.portContextOverride = firstToken;
    REQUIRE(LpcPort_SendMessage(
                &replacement, message, static_cast<ULONG>(sizeof(message)),
                17U, 1U, 0U,
                static_cast<typedef_LpcSyncMsgReplyCallback>(0),
                static_cast<void *>(0)) == STATUS_SUCCESS);
    REQUIRE(LpcPort_ProcessBlockedEvent(&fixture.server) ==
            STATUS_PORT_DISCONNECTED);
    REQUIRE(fixture.callbacks.asyncCalls == 0U);

    g_lpc.portContextOverride = static_cast<PVOID>(replacementEndpoint);
    REQUIRE(LpcPort_SendMessage(
                &replacement, message, static_cast<ULONG>(sizeof(message)),
                18U, 1U, 0U,
                static_cast<typedef_LpcSyncMsgReplyCallback>(0),
                static_cast<void *>(0)) == STATUS_SUCCESS);
    REQUIRE(LpcPort_ProcessBlockedEvent(&fixture.server) ==
            STATUS_PORT_DISCONNECTED);
    REQUIRE(fixture.callbacks.asyncCalls == 0U);

    g_lpc.portContextOverride = static_cast<PVOID>(0);
    REQUIRE(LpcPort_SendMessage(
                &replacement, message, static_cast<ULONG>(sizeof(message)),
                19U, 1U, 0U,
                static_cast<typedef_LpcSyncMsgReplyCallback>(0),
                static_cast<void *>(0)) == STATUS_SUCCESS);
    REQUIRE(LpcPort_ProcessBlockedEvent(&fixture.server) == STATUS_SUCCESS);
    REQUIRE(fixture.callbacks.asyncCalls == 1U);

    LpcPort_Disconnect(&replacement);
}
