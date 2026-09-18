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

/* Keep the public ABI independent of a consuming project's /Gd or /Gz
 * setting.  The macro is shared by the memory-library headers so a build that
 * includes more than one of them still sees one consistent convention.
 *
 * Build contract: an x86 kernel build of allocator.c itself must retain the
 * WDK's default StdCall (/Gz) setting.  Some WDK imports, including
 * ExFreePoolWithTag, inherit the translation unit's default convention even
 * though this library's public entry points are explicitly __cdecl.  x64 and
 * ARM64 have one platform calling convention and are unaffected. */
#ifndef DEVLIB_API_CALL
#if defined(_MSC_VER)
#define DEVLIB_API_CALL __cdecl
#elif defined(__i386__) && defined(_WIN32) && \
      (defined(__GNUC__) || defined(__clang__))
#define DEVLIB_API_CALL __attribute__((__cdecl__))
#else
#define DEVLIB_API_CALL
#endif
#endif

/*
 * Allocates a block with the platform-appropriate backend.  A zero-size
 * request, or a kernel request with a zero pool tag, returns NULL.
 *
 * In kernel mode bUseNonPagedPool must be nonzero for resident memory and
 * zero for paged memory.  The default remains compatible with Windows 10
 * version 1809 and uses ExAllocatePoolWithTag (with NonPagedPoolNx on Windows
 * 8 or newer).  Builds whose minimum target is Windows 10 version 2004 may
 * opt in to ExAllocatePool2 with ALLOCATOR_USE_POOL2=1.  At DISPATCH_LEVEL a
 * paged request is rejected before the system allocator is called.  The
 * caller must free the result with Allocator_Free and must use the same tag
 * supplied to Allocator_Malloc.
 */
#ifdef _KERNEL_MODE
#if defined(ALLOCATOR_HAVE_WDK)
_IRQL_requires_max_(DISPATCH_LEVEL)
_When_(bUseNonPagedPool == 0, _IRQL_requires_max_(APC_LEVEL))
_Ret_maybenull_ _Post_writable_byte_size_(size)
#endif
void *DEVLIB_API_CALL Allocator_Malloc(BOOLEAN bUseNonPagedPool,
                                       size_t size,
                                       ULONG tag);
void DEVLIB_API_CALL Allocator_Free(void *ptr, ULONG tag);
#else
void *DEVLIB_API_CALL Allocator_Malloc(size_t size);
void DEVLIB_API_CALL Allocator_Free(void *ptr);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALLOCATOR_H_INCLUDED */
