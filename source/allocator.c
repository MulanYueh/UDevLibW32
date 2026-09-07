#include "allocator.h"

#ifdef _KERNEL_MODE

/* ExAllocatePool2 was introduced for Windows 10 version 2004 (VB).  Keep the
 * decision based on the driver's target WDK version, not on the host compiler
 * version: old targets must continue using the legacy pool API. */
#if defined(NTDDI_VERSION) && defined(NTDDI_WIN10_VB) && \
    (NTDDI_VERSION >= NTDDI_WIN10_VB) && defined(POOL_FLAG_NON_PAGED) && \
    defined(POOL_FLAG_PAGED)
#define ALLOCATOR_USE_POOL2 1
#endif

#if !defined(ALLOCATOR_HAVE_WDK)
typedef unsigned long allocator_pool_type_t;
extern void *ExAllocatePoolWithTag(allocator_pool_type_t pool_type,
                                   size_t number_of_bytes,
                                   ULONG tag);
extern void ExFreePoolWithTag(void *address, ULONG tag);
#ifndef NonPagedPool
#define NonPagedPool ((allocator_pool_type_t)0)
#endif
#ifndef PagedPool
#define PagedPool ((allocator_pool_type_t)1)
#endif
#endif

void *
Allocator_Malloc(BOOLEAN bUseNonPagedPool, size_t size, ULONG tag)
{
    if (size == 0u || tag == 0u) {
        return (void *)0;
    }

#if defined(ALLOCATOR_USE_POOL2)
    /* Pool2 zeroes memory by default.  The queue initializes its descriptors
     * explicitly, so either backend has identical observable behavior here. */
    return ExAllocatePool2(
        bUseNonPagedPool ? POOL_FLAG_NON_PAGED : POOL_FLAG_PAGED,
        size, tag);
#else
    /* This call is intentional for pre-VB targets.  New WDKs annotate it as
     * deprecated even though it is the correct API for those target systems. */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    {
        void *memory = ExAllocatePoolWithTag(
            bUseNonPagedPool ? NonPagedPool : PagedPool, size, tag);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        return memory;
    }
#endif
}

void
Allocator_Free(void *ptr, ULONG tag)
{
    if (ptr == (void *)0 || tag == 0u) {
        return;
    }

    /* ExFreePoolWithTag remains the broadly available release primitive and
     * can release blocks returned by both ExAllocatePool2 and its predecessor.
     */
    ExFreePoolWithTag(ptr, tag);
}

#else /* user mode */

#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#endif

void *
Allocator_Malloc(size_t size)
{
    if (size == 0u) {
        return (void *)0;
    }

#ifdef _WIN32
    {
        HANDLE process_heap = GetProcessHeap();
        return process_heap == (HANDLE)0
                   ? (void *)0
                   : HeapAlloc(process_heap, 0u, size);
    }
#else
    return malloc(size);
#endif
}

void
Allocator_Free(void *ptr)
{
    if (ptr == (void *)0) {
        return;
    }

#ifdef _WIN32
    {
        HANDLE process_heap = GetProcessHeap();
        if (process_heap != (HANDLE)0) {
            (void)HeapFree(process_heap, 0u, ptr);
        }
    }
#else
    free(ptr);
#endif
}

#endif /* _KERNEL_MODE */
