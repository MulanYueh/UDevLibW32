#ifndef ALLOCATOR_H_INCLUDED
#define ALLOCATOR_H_INCLUDED

/* Keep the public interface usable without pulling a CRT into kernel code. */
#include <stddef.h>

#if defined(_KERNEL_MODE)
/* A normal driver build supplies ntddk.h.  The fallback declarations keep
 * freestanding syntax checks possible when a WDK include path is absent. */
#if defined(__has_include)
#if __has_include(<ntddk.h>)
#include <ntddk.h>
#define ALLOCATOR_HAVE_WDK 1
#endif
#elif defined(_MSC_VER)
#include <ntddk.h>
#define ALLOCATOR_HAVE_WDK 1
#endif

#ifndef ALLOCATOR_HAVE_WDK
typedef unsigned char BOOLEAN;
typedef unsigned long ULONG;
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Allocates a block with the platform-appropriate backend.  A zero-size
 * request, or a kernel request with a zero pool tag, returns NULL.
 *
 * In kernel mode bUseNonPagedPool must be nonzero for resident memory and
 * zero for paged memory.  The implementation selects ExAllocatePool2 when
 * the target WDK is Windows 10 2004 or newer; otherwise it uses
 * ExAllocatePoolWithTag.  The caller must free the result with Allocator_Free
 * and must use the same tag supplied to Allocator_Malloc.
 */
#ifdef _KERNEL_MODE
void *Allocator_Malloc(BOOLEAN bUseNonPagedPool, size_t size, ULONG tag);
void Allocator_Free(void *ptr, ULONG tag);
#else
void *Allocator_Malloc(size_t size);
void Allocator_Free(void *ptr);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALLOCATOR_H_INCLUDED */
