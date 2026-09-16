/*
 * User-mode ALPC demo.
 *
 * This single process contains a named connection-port server and one client.
 * ALPC still creates a separate communication port for the accepted client.
 * Windows queues application traffic on the connection-port receive path, so
 * this single-client demo dispatches that path with ProcessClientEventEx while
 * using the accepted communication handle for replies and close notification.
 *
 * Build with a Windows SDK and the ALPC implementation, for example:
 *   cl /W4 /WX alpc_demo.c alpc_port.c allocator.c libc.c list.c /Fe:alpc_demo.exe
 *
 * Native functions are resolved from ntdll.dll at runtime.  The relative
 * LARGE_INTEGER values below are one-second or five-second 100ns intervals.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "alpc_port.h"

#ifdef __cplusplus
#define ALPC_DEMO_ZERO_INIT {}
#else
#define ALPC_DEMO_ZERO_INIT {0}
#endif

static volatile LONG g_alpc_demo_stop = 0;
static HANDLE g_alpc_demo_ready_event = NULL;
static HANDLE g_alpc_demo_connected_event = NULL;
static HANDLE g_alpc_demo_async_event = NULL;
static HANDLE g_alpc_demo_closed_event = NULL;
static HANDLE g_alpc_demo_connection_done_event = NULL;
static HANDLE g_alpc_demo_client_done_event = NULL;
static HANDLE g_alpc_demo_client_port = NULL;

static int alpc_demo_set_name(ALPC_PORT_NAME *name, const char *value)
{
    size_t length = 0;

    if (!name || !value) {
        return 0;
    }
    while (value[length] != '\0') {
        if (length + 1 >= sizeof(name->name)) {
            return 0;
        }
        ++length;
    }
    memset(name, 0, sizeof(*name));
    memcpy(name->name, value, length + 1);
    return 1;
}

static void alpc_demo_print_status(const char *operation, NTSTATUS status)
{
    if (operation) {
        printf("%s failed: NTSTATUS=0x%08lx\n", operation,
               (unsigned long)(uint32_t)status);
    }
}

static void alpc_demo_print_payload(const char *prefix,
                                    const uint8_t *payload,
                                    ULONG payload_length)
{
    if (prefix) {
        printf("%s", prefix);
    }
    if (payload && payload_length) {
        (void)fwrite(payload, 1, (size_t)payload_length, stdout);
    }
    printf("\n");
}

/* Avoid non-portable function/object pointer casts when compiling the demo
 * with strict C diagnostics.  Windows guarantees a uniform FARPROC size. */
static int alpc_demo_resolve_proc(void *slot,
                                  size_t slot_size,
                                  HMODULE ntdll,
                                  const char *name)
{
    FARPROC procedure = NULL;

    if (!slot || slot_size != sizeof(procedure) || !ntdll || !name) {
        return 0;
    }
    procedure = GetProcAddress(ntdll, name);
    if (!procedure) {
        return 0;
    }
    memcpy(slot, &procedure, sizeof(procedure));
    return 1;
}

static int alpc_demo_resolve_client_apis(HMODULE ntdll,
                                         ALPC_PORT_CLIENT_APIS *api)
{
    if (!ntdll || !api) {
        return 0;
    }
    memset(api, 0, sizeof(*api));
    return alpc_demo_resolve_proc(&api->pfnRtlInitAnsiString,
                                  sizeof(api->pfnRtlInitAnsiString), ntdll,
                                  "RtlInitAnsiString") &&
           alpc_demo_resolve_proc(&api->pfnRtlAnsiStringToUnicodeString,
                                  sizeof(api->pfnRtlAnsiStringToUnicodeString),
                                  ntdll, "RtlAnsiStringToUnicodeString") &&
           alpc_demo_resolve_proc(&api->pfnRtlFreeUnicodeString,
                                  sizeof(api->pfnRtlFreeUnicodeString), ntdll,
                                  "RtlFreeUnicodeString") &&
           alpc_demo_resolve_proc(&api->pfnNtClose, sizeof(api->pfnNtClose),
                                  ntdll, "NtClose") &&
           alpc_demo_resolve_proc(&api->pfnNtAlpcConnectPort,
                                  sizeof(api->pfnNtAlpcConnectPort), ntdll,
                                  "NtAlpcConnectPort") &&
           alpc_demo_resolve_proc(&api->pfnNtAlpcSendWaitReceivePort,
                                  sizeof(api->pfnNtAlpcSendWaitReceivePort),
                                  ntdll, "NtAlpcSendWaitReceivePort") &&
           alpc_demo_resolve_proc(&api->pfnNtAlpcDisconnectPort,
                                  sizeof(api->pfnNtAlpcDisconnectPort), ntdll,
                                  "NtAlpcDisconnectPort");
}

static int alpc_demo_resolve_server_apis(HMODULE ntdll,
                                         ALPC_PORT_SERVER_APIS *api)
{
    if (!ntdll || !api) {
        return 0;
    }
    memset(api, 0, sizeof(*api));
    return alpc_demo_resolve_proc(&api->pfnRtlInitAnsiString,
                                  sizeof(api->pfnRtlInitAnsiString), ntdll,
                                  "RtlInitAnsiString") &&
           alpc_demo_resolve_proc(&api->pfnRtlAnsiStringToUnicodeString,
                                  sizeof(api->pfnRtlAnsiStringToUnicodeString),
                                  ntdll, "RtlAnsiStringToUnicodeString") &&
           alpc_demo_resolve_proc(&api->pfnRtlFreeUnicodeString,
                                  sizeof(api->pfnRtlFreeUnicodeString), ntdll,
                                  "RtlFreeUnicodeString") &&
           alpc_demo_resolve_proc(&api->pfnNtClose, sizeof(api->pfnNtClose),
                                  ntdll, "NtClose") &&
           alpc_demo_resolve_proc(&api->pfnNtAlpcCreatePort,
                                  sizeof(api->pfnNtAlpcCreatePort), ntdll,
                                  "NtAlpcCreatePort") &&
           alpc_demo_resolve_proc(&api->pfnNtAlpcAcceptConnectPort,
                                  sizeof(api->pfnNtAlpcAcceptConnectPort),
                                  ntdll, "NtAlpcAcceptConnectPort") &&
           alpc_demo_resolve_proc(&api->pfnNtAlpcSendWaitReceivePort,
                                  sizeof(api->pfnNtAlpcSendWaitReceivePort),
                                  ntdll, "NtAlpcSendWaitReceivePort") &&
           alpc_demo_resolve_proc(&api->pfnNtAlpcDisconnectPort,
                                  sizeof(api->pfnNtAlpcDisconnectPort), ntdll,
                                  "NtAlpcDisconnectPort");
}

static void alpc_demo_on_pre_connect(uint32_t *response_control_id,
                                     uint8_t *deny,
                                     PVOID callback_context)
{
    (void)callback_context;
    if (response_control_id) {
        *response_control_id += 1U;
    }
    if (deny) {
        *deny = 0U;
    }
    printf("[ALPC server] connection request accepted\n");
}

static void alpc_demo_on_post_connect(HANDLE client_port,
                                      const ALPC_PORT_CLIENT_ID *client_id,
                                      uint32_t control_id,
                                      PVOID callback_context)
{
    (void)callback_context;
    g_alpc_demo_client_port = client_port;
    printf("[ALPC server] communication port=%p controlId=%lu",
           client_port, (unsigned long)control_id);
    if (client_id) {
        printf(" pid=%p tid=%p", client_id->UniqueProcess,
               client_id->UniqueThread);
    }
    printf("\n");
    if (g_alpc_demo_connected_event) {
        (void)SetEvent(g_alpc_demo_connected_event);
    }
}

static NTSTATUS alpc_demo_on_sync_request(HANDLE client_port,
                                          const ALPC_PORT_CLIENT_ID *client_id,
                                          uint32_t control_id,
                                          uint8_t *payload,
                                          ULONG *payload_length,
                                          ULONG max_payload_length,
                                          PVOID callback_context)
{
    ULONG index = 0;

    (void)client_port;
    (void)client_id;
    (void)max_payload_length;
    (void)callback_context;
    printf("[ALPC server] sync request controlId=%lu: ",
           (unsigned long)control_id);
    if (payload_length) {
        alpc_demo_print_payload("", payload, *payload_length);
        for (index = 0; payload && index < *payload_length; ++index) {
            if (payload[index] >= (uint8_t)'a' &&
                payload[index] <= (uint8_t)'z') {
                payload[index] = (uint8_t)(payload[index] - (uint8_t)'a' +
                                           (uint8_t)'A');
            }
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS alpc_demo_on_async_request(HANDLE client_port,
                                           const ALPC_PORT_CLIENT_ID *client_id,
                                           uint32_t control_id,
                                           const uint8_t *payload,
                                           ULONG payload_length,
                                           PVOID callback_context)
{
    (void)client_port;
    (void)client_id;
    (void)callback_context;
    printf("[ALPC server] async datagram controlId=%lu: ",
           (unsigned long)control_id);
    alpc_demo_print_payload("", payload, payload_length);
    if (g_alpc_demo_async_event) {
        (void)SetEvent(g_alpc_demo_async_event);
    }
    return STATUS_SUCCESS;
}

static void alpc_demo_on_close(HANDLE client_port,
                               const ALPC_PORT_CLIENT_ID *client_id,
                               PVOID callback_context)
{
    (void)client_id;
    (void)callback_context;
    printf("[ALPC server] communication port closed: %p\n", client_port);
    if (g_alpc_demo_closed_event) {
        (void)SetEvent(g_alpc_demo_closed_event);
    }
}

static DWORD WINAPI alpc_demo_connection_thread(LPVOID parameter)
{
    PALPC_PORT_SERVER_CONTEXT server = (PALPC_PORT_SERVER_CONTEXT)parameter;
    LARGE_INTEGER timeout = ALPC_DEMO_ZERO_INIT;
    NTSTATUS status = STATUS_SUCCESS;

    if (g_alpc_demo_ready_event) {
        (void)SetEvent(g_alpc_demo_ready_event);
    }
    while (server && InterlockedCompareExchange(&g_alpc_demo_stop, 0, 0) == 0) {
        timeout.QuadPart = -10000000LL;
        status = AlpcPort_ProcessBlockedEventEx(server, &timeout);
        if (status == STATUS_TIMEOUT) {
            continue;
        }
        if (status == STATUS_PORT_DISCONNECTED) {
            break;
        }
        if (!NT_SUCCESS(status)) {
            alpc_demo_print_status("AlpcPort_ProcessBlockedEventEx", status);
            Sleep(1);
        }
        /* This demo owns one client.  Once its communication port is
         * published, the connection worker can stop and the dedicated client
         * worker consumes the connection-port receive queue.  A production
         * multi-client dispatcher should route messages using native ALPC
         * message attributes before invoking the per-client callbacks. */
        if (g_alpc_demo_client_port) {
            break;
        }
    }
    if (g_alpc_demo_connection_done_event) {
        (void)SetEvent(g_alpc_demo_connection_done_event);
    }
    return 0;
}

static DWORD WINAPI alpc_demo_client_thread(LPVOID parameter)
{
    PALPC_PORT_SERVER_CONTEXT server = (PALPC_PORT_SERVER_CONTEXT)parameter;
    HANDLE client_port = NULL;
    LARGE_INTEGER timeout = ALPC_DEMO_ZERO_INIT;
    NTSTATUS status = STATUS_SUCCESS;

    if (server) {
        client_port = server->connectionPortHandle;
    }
    while (server && client_port &&
           InterlockedCompareExchange(&g_alpc_demo_stop, 0, 0) == 0) {
        timeout.QuadPart = -10000000LL;
        status = AlpcPort_ProcessClientEventEx(server, client_port, &timeout);
        if (status == STATUS_TIMEOUT) {
            continue;
        }
        if (status == STATUS_PORT_DISCONNECTED ||
            status == (NTSTATUS)STATUS_INVALID_PARAMETER) {
            break;
        }
        if (!NT_SUCCESS(status)) {
            alpc_demo_print_status("AlpcPort_ProcessClientEventEx", status);
            Sleep(1);
        }
    }
    if (g_alpc_demo_client_done_event) {
        (void)SetEvent(g_alpc_demo_client_done_event);
    }
    return 0;
}

static NTSTATUS NTAPI alpc_demo_on_reply(const uint8_t *buffer,
                                         ULONG length,
                                         PVOID callback_context)
{
    (void)callback_context;
    printf("[ALPC client] reply: ");
    alpc_demo_print_payload("", buffer, length);
    return STATUS_SUCCESS;
}

int main(void)
{
    HMODULE ntdll = NULL;
    ALPC_PORT_CLIENT_APIS client_apis = ALPC_DEMO_ZERO_INIT;
    ALPC_PORT_SERVER_APIS server_apis = ALPC_DEMO_ZERO_INIT;
    ALPC_PORT_SERVER_CONFIG server_config = ALPC_DEMO_ZERO_INIT;
    ALPC_PORT_SERVER_CONTEXT server_context = ALPC_DEMO_ZERO_INIT;
    ALPC_PORT_SERVER_EVENTS events = ALPC_DEMO_ZERO_INIT;
    ALPC_PORT_CLIENT_CONFIG client_config = ALPC_DEMO_ZERO_INIT;
    ALPC_PORT_CLIENT_CONTEXT client_context = ALPC_DEMO_ZERO_INIT;
    HANDLE connection_thread = NULL;
    HANDLE client_thread = NULL;
    DWORD wait_result = WAIT_FAILED;
    LARGE_INTEGER connect_timeout = ALPC_DEMO_ZERO_INIT;
    LARGE_INTEGER send_timeout = ALPC_DEMO_ZERO_INIT;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    int server_created = 0;
    int client_connected = 0;
    int result = 1;
    static const uint8_t sync_message[] = "hello from ALPC";
    static const uint8_t async_message[] = "one-way ALPC datagram";

    g_alpc_demo_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_alpc_demo_connected_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_alpc_demo_async_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_alpc_demo_closed_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_alpc_demo_connection_done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_alpc_demo_client_done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_alpc_demo_ready_event || !g_alpc_demo_connected_event ||
        !g_alpc_demo_async_event || !g_alpc_demo_closed_event ||
        !g_alpc_demo_connection_done_event || !g_alpc_demo_client_done_event) {
        printf("CreateEventW failed: error=%lu\n", (unsigned long)GetLastError());
        goto cleanup;
    }

    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll || !alpc_demo_resolve_client_apis(ntdll, &client_apis) ||
        !alpc_demo_resolve_server_apis(ntdll, &server_apis)) {
        printf("Could not resolve the required Native ALPC exports\n");
        goto cleanup;
    }
    if (!alpc_demo_set_name(&server_config.portName,
                            "\\RPC Control\\AlpcPortDemo")) {
        printf("Invalid ALPC port name\n");
        goto cleanup;
    }
    server_config.maxMessageLength = ALPC_PORT_DEFAULT_MAX_MESSAGE_LENGTH;
    server_config.maxClients = 1U;
    server_config.api = server_apis;
    status = AlpcPort_ServerCreate(&server_config, &server_context);
    if (!NT_SUCCESS(status)) {
        alpc_demo_print_status("AlpcPort_ServerCreate", status);
        goto cleanup;
    }
    server_created = 1;

    events.onPreConnect = alpc_demo_on_pre_connect;
    events.onPostConnect = alpc_demo_on_post_connect;
    events.onSyncRequest = alpc_demo_on_sync_request;
    events.onAsyncRequest = alpc_demo_on_async_request;
    events.onClose = alpc_demo_on_close;
    if (!AlpcPort_Register_ServerEvtCallback(&server_context, &events)) {
        printf("AlpcPort_Register_ServerEvtCallback failed\n");
        goto cleanup;
    }
    connection_thread = CreateThread(NULL, 0, alpc_demo_connection_thread,
                                     &server_context, 0, NULL);
    if (!connection_thread) {
        printf("CreateThread failed: error=%lu\n", (unsigned long)GetLastError());
        goto cleanup;
    }
    wait_result = WaitForSingleObject(g_alpc_demo_ready_event, 5000);
    if (wait_result != WAIT_OBJECT_0) {
        printf("connection worker did not start\n");
        goto cleanup;
    }

    connect_timeout.QuadPart = -50000000LL;
    client_config.portName = server_config.portName;
    client_config.helloId = 0x300U;
    client_config.maxMessageLength = ALPC_PORT_DEFAULT_MAX_MESSAGE_LENGTH;
    client_config.api = client_apis;
    client_config.connectTimeout = &connect_timeout;
    status = AlpcPort_Connect(&client_config, &client_context);
    if (!NT_SUCCESS(status)) {
        alpc_demo_print_status("AlpcPort_Connect", status);
        goto cleanup;
    }
    client_connected = 1;
    wait_result = WaitForSingleObject(g_alpc_demo_connected_event, 5000);
    if (wait_result != WAIT_OBJECT_0 || !g_alpc_demo_client_port) {
        printf("server did not publish a communication port\n");
        goto cleanup;
    }
    printf("[ALPC client] connected, response controlId=%lu\n",
           (unsigned long)client_context.negotiatedControlId);

    client_thread = CreateThread(NULL, 0, alpc_demo_client_thread,
                                 &server_context, 0, NULL);
    if (!client_thread) {
        printf("CreateThread(client) failed: error=%lu\n",
               (unsigned long)GetLastError());
        goto cleanup;
    }
    send_timeout.QuadPart = -50000000LL;
    status = AlpcPort_SendMessage(&client_context, sync_message,
                                  (ULONG)(sizeof(sync_message) - 1U),
                                  0x400U, 0U, alpc_demo_on_reply, NULL,
                                  &send_timeout);
    if (!NT_SUCCESS(status)) {
        alpc_demo_print_status("AlpcPort_SendMessage(sync)", status);
        goto cleanup;
    }
    status = AlpcPort_SendMessage(&client_context, async_message,
                                  (ULONG)(sizeof(async_message) - 1U),
                                  0x401U, ALPC_PORT_SEND_FLAG_ASYNC, NULL,
                                  NULL, &send_timeout);
    if (!NT_SUCCESS(status)) {
        alpc_demo_print_status("AlpcPort_SendMessage(async)", status);
        goto cleanup;
    }
    wait_result = WaitForSingleObject(g_alpc_demo_async_event, 5000);
    if (wait_result != WAIT_OBJECT_0) {
        printf("server did not receive the asynchronous datagram\n");
        goto cleanup;
    }

    AlpcPort_Disconnect(&client_context);
    client_connected = 0;
    wait_result = WaitForSingleObject(g_alpc_demo_closed_event, 5000);
    if (wait_result != WAIT_OBJECT_0) {
        printf("server close callback was not observed before timeout\n");
    }
    (void)InterlockedExchange(&g_alpc_demo_stop, 1);
    (void)WaitForSingleObject(g_alpc_demo_client_done_event, 5000);
    (void)WaitForSingleObject(g_alpc_demo_connection_done_event, 5000);
    result = 0;

cleanup:
    if (client_connected) {
        AlpcPort_Disconnect(&client_context);
    }
    (void)InterlockedExchange(&g_alpc_demo_stop, 1);
    if (client_thread) {
        (void)WaitForSingleObject(g_alpc_demo_client_done_event, 5000);
        CloseHandle(client_thread);
        client_thread = NULL;
    }
    if (connection_thread) {
        (void)WaitForSingleObject(g_alpc_demo_connection_done_event, 5000);
        CloseHandle(connection_thread);
        connection_thread = NULL;
    }
    if (server_created) {
        AlpcPort_ServerClose(&server_context);
    }
    if (g_alpc_demo_ready_event) {
        CloseHandle(g_alpc_demo_ready_event);
        g_alpc_demo_ready_event = NULL;
    }
    if (g_alpc_demo_connected_event) {
        CloseHandle(g_alpc_demo_connected_event);
        g_alpc_demo_connected_event = NULL;
    }
    if (g_alpc_demo_async_event) {
        CloseHandle(g_alpc_demo_async_event);
        g_alpc_demo_async_event = NULL;
    }
    if (g_alpc_demo_closed_event) {
        CloseHandle(g_alpc_demo_closed_event);
        g_alpc_demo_closed_event = NULL;
    }
    if (g_alpc_demo_connection_done_event) {
        CloseHandle(g_alpc_demo_connection_done_event);
        g_alpc_demo_connection_done_event = NULL;
    }
    if (g_alpc_demo_client_done_event) {
        CloseHandle(g_alpc_demo_client_done_event);
        g_alpc_demo_client_done_event = NULL;
    }
    return result;
}
