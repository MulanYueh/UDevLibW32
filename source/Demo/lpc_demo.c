/*
 * User-mode LPC demo.
 *
 * The demo keeps a server and a client in one process so it is easy to run,
 * but the two sides communicate through the real named Native LPC port.  A
 * production service normally puts the client in another process.  The
 * Native API table is resolved at runtime from ntdll.dll; no private ntdll
 * import library is required.
 *
 * Build with a Windows SDK and the LPC implementation, for example:
 *   cl /W4 /WX lpc_demo.c lpc_port.c allocator.c libc.c list.c /Fe:lpc_demo.exe
 *
 * The server worker uses a one-second relative timeout.  LPC's
 * NtRequestWaitReplyPort has no timeout/cancel parameter, so only the server
 * receive loop is timed; ALPC provides an end-to-end timeout in alpc_demo.c.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "lpc_port.h"

#ifdef __cplusplus
#define LPC_DEMO_ZERO_INIT {}
#else
#define LPC_DEMO_ZERO_INIT {0}
#endif

static volatile LONG g_lpc_demo_stop = 0;
static HANDLE g_lpc_demo_ready_event = NULL;
static HANDLE g_lpc_demo_async_event = NULL;
static HANDLE g_lpc_demo_done_event = NULL;

static int lpc_demo_set_name(LPC_PORT_NAME *name, const char *value)
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

static void lpc_demo_print_status(const char *operation, NTSTATUS status)
{
    if (operation) {
        printf("%s failed: NTSTATUS=0x%08lx\n", operation,
               (unsigned long)(uint32_t)status);
    }
}

static void lpc_demo_print_payload(const char *prefix,
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

/* GetProcAddress returns FARPROC.  Copying its representation into the
 * already-typed slot avoids non-portable function/object pointer casts under
 * strict C diagnostics while retaining the Windows ABI calling convention. */
static int lpc_demo_resolve_proc(void *slot,
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

static int lpc_demo_resolve_client_apis(HMODULE ntdll, LPC_CLIENT_APIS *api)
{
    if (!ntdll || !api) {
        return 0;
    }
    memset(api, 0, sizeof(*api));
    return lpc_demo_resolve_proc(&api->pfnRtlInitAnsiString,
                                 sizeof(api->pfnRtlInitAnsiString), ntdll,
                                 "RtlInitAnsiString") &&
           lpc_demo_resolve_proc(&api->pfnNtCreateSection,
                                 sizeof(api->pfnNtCreateSection), ntdll,
                                 "NtCreateSection") &&
           lpc_demo_resolve_proc(&api->pfnNtConnectPort,
                                 sizeof(api->pfnNtConnectPort), ntdll,
                                 "NtConnectPort") &&
           lpc_demo_resolve_proc(&api->pfnNtClose, sizeof(api->pfnNtClose),
                                 ntdll, "NtClose") &&
           lpc_demo_resolve_proc(&api->pfnRtlAnsiStringToUnicodeString,
                                 sizeof(api->pfnRtlAnsiStringToUnicodeString),
                                 ntdll, "RtlAnsiStringToUnicodeString") &&
           lpc_demo_resolve_proc(&api->pfnRtlFreeUnicodeString,
                                 sizeof(api->pfnRtlFreeUnicodeString), ntdll,
                                 "RtlFreeUnicodeString") &&
           lpc_demo_resolve_proc(&api->pfnNtRequestPort,
                                 sizeof(api->pfnNtRequestPort), ntdll,
                                 "NtRequestPort") &&
           lpc_demo_resolve_proc(&api->pfnNtRequestWaitReplyPort,
                                 sizeof(api->pfnNtRequestWaitReplyPort), ntdll,
                                 "NtRequestWaitReplyPort");
}

static int lpc_demo_resolve_server_apis(HMODULE ntdll, LPC_SERVER_APIS *api)
{
    if (!ntdll || !api) {
        return 0;
    }
    memset(api, 0, sizeof(*api));
    return lpc_demo_resolve_proc(&api->pfnRtlInitAnsiString,
                                 sizeof(api->pfnRtlInitAnsiString), ntdll,
                                 "RtlInitAnsiString") &&
           lpc_demo_resolve_proc(&api->pfnNtCreatePort,
                                 sizeof(api->pfnNtCreatePort), ntdll,
                                 "NtCreatePort") &&
           lpc_demo_resolve_proc(&api->pfnNtClose, sizeof(api->pfnNtClose),
                                 ntdll, "NtClose") &&
           lpc_demo_resolve_proc(&api->pfnRtlAnsiStringToUnicodeString,
                                 sizeof(api->pfnRtlAnsiStringToUnicodeString),
                                 ntdll, "RtlAnsiStringToUnicodeString") &&
           lpc_demo_resolve_proc(&api->pfnRtlFreeUnicodeString,
                                 sizeof(api->pfnRtlFreeUnicodeString), ntdll,
                                 "RtlFreeUnicodeString") &&
           lpc_demo_resolve_proc(&api->pfnNtReplyWaitReceivePort,
                                 sizeof(api->pfnNtReplyWaitReceivePort), ntdll,
                                 "NtReplyWaitReceivePort") &&
           lpc_demo_resolve_proc(&api->pfnNtReplyWaitReceivePortEx,
                                 sizeof(api->pfnNtReplyWaitReceivePortEx),
                                 ntdll, "NtReplyWaitReceivePortEx") &&
           lpc_demo_resolve_proc(&api->pfnNtAcceptConnectPort,
                                 sizeof(api->pfnNtAcceptConnectPort), ntdll,
                                 "NtAcceptConnectPort") &&
           lpc_demo_resolve_proc(&api->pfnNtCompleteConnectPort,
                                 sizeof(api->pfnNtCompleteConnectPort), ntdll,
                                 "NtCompleteConnectPort") &&
           lpc_demo_resolve_proc(&api->pfnNtReplyPort,
                                 sizeof(api->pfnNtReplyPort), ntdll,
                                 "NtReplyPort");
}

static void lpc_demo_on_pre_connect(uint32_t *response_control_id,
                                     uint8_t *deny)
{
    if (response_control_id) {
        *response_control_id += 1U;
    }
    if (deny) {
        *deny = 0U;
    }
    printf("[LPC server] connection request accepted\n");
}

static void lpc_demo_on_post_connect(HANDLE client_handle, uint32_t control_id)
{
    printf("[LPC server] client connected: handle=%p controlId=%lu\n",
           client_handle, (unsigned long)control_id);
}

static void lpc_demo_on_sync_request(HANDLE client_handle,
                                     uint32_t control_id,
                                     uint8_t *message,
                                     uint32_t message_length,
                                     uint64_t max_size)
{
    uint32_t index = 0;

    (void)client_handle;
    (void)max_size;
    printf("[LPC server] sync request controlId=%lu: ",
           (unsigned long)control_id);
    lpc_demo_print_payload("", message, (ULONG)message_length);
    for (index = 0; index < message_length; ++index) {
        if (message[index] >= (uint8_t)'a' && message[index] <= (uint8_t)'z') {
            message[index] = (uint8_t)(message[index] - (uint8_t)'a' + (uint8_t)'A');
        }
    }
}

static void lpc_demo_on_async_request(HANDLE client_handle,
                                      uint32_t control_id,
                                      uint8_t *message,
                                      uint32_t message_length,
                                      uint64_t max_size)
{
    (void)client_handle;
    (void)max_size;
    printf("[LPC server] async datagram controlId=%lu: ",
           (unsigned long)control_id);
    lpc_demo_print_payload("", message, (ULONG)message_length);
    if (g_lpc_demo_async_event) {
        (void)SetEvent(g_lpc_demo_async_event);
    }
}

static void lpc_demo_on_close(HANDLE client_handle)
{
    printf("[LPC server] client disconnected: handle=%p\n", client_handle);
    (void)InterlockedExchange(&g_lpc_demo_stop, 1);
}

static DWORD WINAPI lpc_demo_server_thread(LPVOID parameter)
{
    PLPC_SERVER_CONTEXT server = (PLPC_SERVER_CONTEXT)parameter;
    LARGE_INTEGER timeout = LPC_DEMO_ZERO_INIT;
    NTSTATUS status = STATUS_SUCCESS;

    if (g_lpc_demo_ready_event) {
        (void)SetEvent(g_lpc_demo_ready_event);
    }
    while (server && InterlockedCompareExchange(&g_lpc_demo_stop, 0, 0) == 0) {
        timeout.QuadPart = -10000000LL; /* one second, relative 100ns units */
        status = LpcPort_ProcessBlockedEventEx(server, &timeout);
        if (status == STATUS_TIMEOUT) {
            continue;
        }
        if (status == STATUS_PORT_DISCONNECTED) {
            break;
        }
        if (!NT_SUCCESS(status)) {
            lpc_demo_print_status("LpcPort_ProcessBlockedEventEx", status);
            Sleep(1);
        }
    }
    if (g_lpc_demo_done_event) {
        (void)SetEvent(g_lpc_demo_done_event);
    }
    return 0;
}

static NTSTATUS NTAPI lpc_demo_on_reply(PVOID buffer, ULONG length, PVOID context)
{
    (void)context;
    printf("[LPC client] reply: ");
    lpc_demo_print_payload("", (const uint8_t *)buffer, length);
    return STATUS_SUCCESS;
}

int main(void)
{
    HMODULE ntdll = NULL;
    LPC_CLIENT_APIS client_apis = LPC_DEMO_ZERO_INIT;
    LPC_SERVER_APIS server_apis = LPC_DEMO_ZERO_INIT;
    LPC_SERVER_CONFIG server_config = LPC_DEMO_ZERO_INIT;
    LPC_SERVER_CONTEXT server_context = LPC_DEMO_ZERO_INIT;
    LPC_SERVER_EVT_CONTEXT events = LPC_DEMO_ZERO_INIT;
    LPC_CLIENT_CONFIG client_config = LPC_DEMO_ZERO_INIT;
    LPC_CLIENT_CONTEXT client_context = LPC_DEMO_ZERO_INIT;
    HANDLE server_thread = NULL;
    DWORD wait_result = WAIT_FAILED;
    uint32_t response_id = 0;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    int server_created = 0;
    int client_connected = 0;
    int result = 1;
    static const uint8_t sync_message[] = "hello from LPC";
    static const uint8_t async_message[] = "one-way LPC datagram";

    g_lpc_demo_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_lpc_demo_async_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_lpc_demo_done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_lpc_demo_ready_event || !g_lpc_demo_async_event ||
        !g_lpc_demo_done_event) {
        printf("CreateEventW failed: error=%lu\n", (unsigned long)GetLastError());
        goto cleanup;
    }

    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll || !lpc_demo_resolve_client_apis(ntdll, &client_apis) ||
        !lpc_demo_resolve_server_apis(ntdll, &server_apis)) {
        printf("Could not resolve the required Native LPC exports\n");
        goto cleanup;
    }
    if (!lpc_demo_set_name(&server_config.LpcName, "\\RPC Control\\LpcPortDemo")) {
        printf("Invalid LPC port name\n");
        goto cleanup;
    }
    server_config.maxClients = 1U;
    server_config.api = server_apis;
    status = LpcPort_ServerCreate(&server_config, &server_context);
    if (!NT_SUCCESS(status)) {
        lpc_demo_print_status("LpcPort_ServerCreate", status);
        goto cleanup;
    }
    server_created = 1;

    events.onPreConnect = lpc_demo_on_pre_connect;
    events.onPostConnect = lpc_demo_on_post_connect;
    events.onSyncRequest = lpc_demo_on_sync_request;
    events.onAsyncRequest = lpc_demo_on_async_request;
    events.onClose = lpc_demo_on_close;
    if (!LpcPort_Register_ServerEvtCallback(&server_context, &events)) {
        printf("LpcPort_Register_ServerEvtCallback failed\n");
        goto cleanup;
    }
    server_thread = CreateThread(NULL, 0, lpc_demo_server_thread,
                                 &server_context, 0, NULL);
    if (!server_thread) {
        printf("CreateThread failed: error=%lu\n", (unsigned long)GetLastError());
        goto cleanup;
    }
    wait_result = WaitForSingleObject(g_lpc_demo_ready_event, 5000);
    if (wait_result != WAIT_OBJECT_0) {
        printf("server worker did not start\n");
        goto cleanup;
    }

    client_config.HelloId = 0x100U;
    client_config.LpcName = server_config.LpcName;
    client_config.lpRespID = &response_id;
    client_config.api = client_apis;
    status = LpcPort_Connect(&client_config, &client_context);
    if (!NT_SUCCESS(status)) {
        lpc_demo_print_status("LpcPort_Connect", status);
        goto cleanup;
    }
    client_connected = 1;
    printf("[LPC client] connected, response controlId=%lu\n",
           (unsigned long)response_id);

    status = LpcPort_SendMessage(&client_context, sync_message,
                                 (ULONG)(sizeof(sync_message) - 1U),
                                 0x200U, 0U, 0U, lpc_demo_on_reply, NULL);
    if (!NT_SUCCESS(status)) {
        lpc_demo_print_status("LpcPort_SendMessage(sync)", status);
        goto cleanup;
    }
    status = LpcPort_SendMessage(&client_context, async_message,
                                 (ULONG)(sizeof(async_message) - 1U),
                                 0x201U, 1U, 0U, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        lpc_demo_print_status("LpcPort_SendMessage(async)", status);
        goto cleanup;
    }
    wait_result = WaitForSingleObject(g_lpc_demo_async_event, 5000);
    if (wait_result != WAIT_OBJECT_0) {
        printf("server did not receive the asynchronous datagram\n");
        goto cleanup;
    }

    /* Disconnect wakes the server receive loop and triggers onClose. */
    LpcPort_Disconnect(&client_context);
    client_connected = 0;
    wait_result = WaitForSingleObject(g_lpc_demo_done_event, 5000);
    if (wait_result != WAIT_OBJECT_0) {
        (void)InterlockedExchange(&g_lpc_demo_stop, 1);
    }
    result = 0;

cleanup:
    if (client_connected) {
        LpcPort_Disconnect(&client_context);
    }
    (void)InterlockedExchange(&g_lpc_demo_stop, 1);
    if (server_thread) {
        (void)WaitForSingleObject(server_thread, 5000);
        CloseHandle(server_thread);
        server_thread = NULL;
    }
    if (server_created) {
        LpcPort_ServerClose(&server_context);
    }
    if (g_lpc_demo_ready_event) {
        CloseHandle(g_lpc_demo_ready_event);
        g_lpc_demo_ready_event = NULL;
    }
    if (g_lpc_demo_async_event) {
        CloseHandle(g_lpc_demo_async_event);
        g_lpc_demo_async_event = NULL;
    }
    if (g_lpc_demo_done_event) {
        CloseHandle(g_lpc_demo_done_event);
        g_lpc_demo_done_event = NULL;
    }
    return result;
}
