#include "allocator.h"

#ifdef _KERNEL_MODE

/* The shipping default remains compatible with Windows 10 version 1809, so
 * it deliberately uses the legacy API.  A target whose minimum OS is Windows
 * 10 version 2004 (VB) or newer may opt in with /DALLOCATOR_USE_POOL2=1. */
#ifndef ALLOCATOR_USE_POOL2
#define ALLOCATOR_USE_POOL2 0
#endif

#if ALLOCATOR_USE_POOL2 && \
    (!defined(NTDDI_VERSION) || !defined(NTDDI_WIN10_VB) || \
     (NTDDI_VERSION < NTDDI_WIN10_VB) || !defined(POOL_FLAG_NON_PAGED) || \
     !defined(POOL_FLAG_PAGED))
#error ALLOCATOR_USE_POOL2 requires a Windows 10 version 2004 or newer target
#endif

#if !defined(ALLOCATOR_HAVE_WDK)
typedef unsigned long allocator_pool_type_t;
typedef unsigned char KIRQL;
extern void *ExAllocatePoolWithTag(allocator_pool_type_t pool_type,
                                   size_t number_of_bytes,
                                   ULONG tag);
extern void ExFreePoolWithTag(void *address, ULONG tag);
extern KIRQL KeGetCurrentIrql(void);
#ifndef PASSIVE_LEVEL
#define PASSIVE_LEVEL ((KIRQL)0)
#endif
#ifndef DISPATCH_LEVEL
#define DISPATCH_LEVEL ((KIRQL)2)
#endif
#ifndef NonPagedPool
#define NonPagedPool ((allocator_pool_type_t)0)
#endif
#ifndef PagedPool
#define PagedPool ((allocator_pool_type_t)1)
#endif
#endif

/* NonPagedPool is executable on the legacy API unless the whole driver opts
 * in to the NX remapping.  Prefer the explicit NX type whenever the selected
 * target is Windows 8 or newer, while retaining the old value for genuinely
 * down-level builds and freestanding syntax checks. */
#if defined(ALLOCATOR_HAVE_WDK) && defined(NTDDI_VERSION) && \
    defined(NTDDI_WIN8) && (NTDDI_VERSION >= NTDDI_WIN8)
#define ALLOCATOR_LEGACY_NONPAGED_POOL NonPagedPoolNx
#else
#define ALLOCATOR_LEGACY_NONPAGED_POOL NonPagedPool
#endif

#if defined(ALLOCATOR_HAVE_WDK)
_Use_decl_annotations_
#endif
void *
DEVLIB_API_CALL Allocator_Malloc(BOOLEAN bUseNonPagedPool,
                                 size_t size,
                                 ULONG tag)
{
#ifdef _KERNEL_MODE
    KIRQL current_irql = PASSIVE_LEVEL;
#endif

    if (size == 0u || tag == 0u) {
        return (void *)0;
    }

    /* PagedPool allocation is illegal at DPC.  Reject it before entering
       either ExAllocatePool2 or the legacy ExAllocatePoolWithTag path. */
    current_irql = KeGetCurrentIrql();
    if (current_irql > DISPATCH_LEVEL ||
        (current_irql == DISPATCH_LEVEL && !bUseNonPagedPool)) {
        return (void *)0;
    }

#if ALLOCATOR_USE_POOL2
    /* Pool2 zeroes memory by default.  Allocator_Malloc deliberately exposes
     * no initialization guarantee so callers remain correct on either
     * backend. */
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
            bUseNonPagedPool ? ALLOCATOR_LEGACY_NONPAGED_POOL : PagedPool,
            size, tag);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        return memory;
    }
#endif
}

void
DEVLIB_API_CALL Allocator_Free(void *ptr, ULONG tag)
{
    if (ptr == (void *)0 || tag == 0u) {
        return;
    }

    /* ExFreePoolWithTag remains the broadly available release primitive and
     * can release blocks returned by both ExAllocatePool2 and its predecessor.
     */
    ExFreePoolWithTag(ptr, tag);
}

#undef ALLOCATOR_LEGACY_NONPAGED_POOL

#else /* user mode */

#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#endif

void *
DEVLIB_API_CALL Allocator_Malloc(size_t size)
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
DEVLIB_API_CALL Allocator_Free(void *ptr)
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
