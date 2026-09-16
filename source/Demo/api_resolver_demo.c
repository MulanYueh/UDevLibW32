/*
 * User-mode demo for ApiResolver_GetFuncAddress.
 *
 * The resolver and the private CRT must be compiled as C and linked together:
 *
 *   gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -I. \
 *       api_resolver_demo.c api_resolver.c libc.c -lkernel32 \
 *       -o api_resolver_demo.exe
 *
 * Optional argument:
 *
 *   api_resolver_demo.exe GetSystemTimeAsFileTime
 *   api_resolver_demo.exe "#123"
 *
 * The second form demonstrates ordinal syntax.  Ordinals are module-version
 * specific, so the demo intentionally does not hard-code one for kernel32.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "api_resolver.h"
#include "libc.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef DWORD (WINAPI *DEMO_GET_CURRENT_PROCESS_ID)(void);
typedef ULONGLONG (WINAPI *DEMO_GET_TICK_COUNT64)(void);
typedef HMODULE (WINAPI *DEMO_GET_MODULE_HANDLE_W)(LPCWSTR module_name);

/*
 * ApiResolver_GetFuncAddress returns an integer large enough to hold a native
 * address.  Copying its representation into a correctly typed function
 * pointer avoids hiding the calling convention in an untyped cast.
 */
static int
demo_bind_function(
    void *function_pointer,
    size_t function_pointer_size,
    API_RESOLVER_ADDRESS address
    )
{
    if (!function_pointer || !address ||
        function_pointer_size != sizeof(address)) {
        return 0;
    }

    (void)libc_memcpy(function_pointer, &address, sizeof(address));
    return 1;
}

static int
demo_resolve(
    HMODULE module,
    const char *name,
    void *function_pointer,
    size_t function_pointer_size
    )
{
    API_RESOLVER_ADDRESS address = 0;

    address = ApiResolver_GetFuncAddress((void *)module, name);
    if (!address) {
        fprintf(stderr, "resolve failed: %s\n", name ? name : "(null)");
        return 0;
    }
    if (!demo_bind_function(function_pointer, function_pointer_size, address)) {
        fprintf(stderr, "unsupported function-pointer representation: %s\n",
                name);
        return 0;
    }

    printf("resolved %-20s -> 0x%" PRIxPTR "\n",
           name, (uintptr_t)address);
    return 1;
}

int
main(
    int argc,
    char **argv
    )
{
    HMODULE kernel32 = NULL;
    HMODULE ntdll = NULL;
    DEMO_GET_CURRENT_PROCESS_ID get_current_process_id = NULL;
    DEMO_GET_TICK_COUNT64 get_tick_count64 = NULL;
    DEMO_GET_MODULE_HANDLE_W get_module_handle_w = NULL;
    API_RESOLVER_ADDRESS missing = 0;
    API_RESOLVER_ADDRESS requested = 0;

    /* Base must identify an image already mapped into this address space. */
    kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
        fprintf(stderr, "GetModuleHandleW(kernel32.dll) failed: %lu\n",
                (unsigned long)GetLastError());
        return 1;
    }
    printf("kernel32.dll base       -> %p\n", (void *)kernel32);

    if (!demo_resolve(kernel32, "GetCurrentProcessId",
                      &get_current_process_id,
                      sizeof(get_current_process_id)) ||
        !demo_resolve(kernel32, "GetTickCount64",
                      &get_tick_count64,
                      sizeof(get_tick_count64)) ||
        !demo_resolve(kernel32, "GetModuleHandleW",
                      &get_module_handle_w,
                      sizeof(get_module_handle_w))) {
        return 2;
    }

    printf("GetCurrentProcessId()   -> %lu\n",
           (unsigned long)get_current_process_id());
    printf("GetTickCount64()        -> %" PRIu64 " ms\n",
           (uint64_t)get_tick_count64());

    /* Forwarded exports require no special caller-side handling. */
    ntdll = get_module_handle_w(L"ntdll.dll");
    if (!ntdll) {
        fputs("resolved GetModuleHandleW could not find ntdll.dll\n", stderr);
        return 3;
    }
    printf("GetModuleHandleW(ntdll) -> %p\n", (void *)ntdll);

    /* Absence is reported as zero; the resolver does not set GetLastError. */
    missing = ApiResolver_GetFuncAddress(
        (void *)kernel32, "ApiResolver_Demo_Missing_Export");
    printf("missing export          -> 0x%" PRIxPTR " (expected 0)\n",
           (uintptr_t)missing);
    if (missing) {
        return 4;
    }

    if (argc > 1) {
        requested = ApiResolver_GetFuncAddress((void *)kernel32, argv[1]);
        printf("requested %-18s -> 0x%" PRIxPTR "\n",
               argv[1], (uintptr_t)requested);
        if (!requested) {
            return 5;
        }
    }

    return 0;
}

