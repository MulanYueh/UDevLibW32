/*
 * Windows user-mode/kernel-mode memory pool.
 *
 * The public API is intentionally small.  All implementation details,
 * including the page lists and synchronization object, stay private to
 * mempool.c.
 *
 * The user-mode implementation uses libc_memset/libc_memcpy from the team's
 * private CRT; link libc.c with mempool.c.  Kernel backing allocations route
 * through allocator.c, which selects ExAllocatePool2 or the legacy pool API
 * according to the driver's target WDK version.  Kernel mode continues to
 * use the WDK Rtl* memory primitives and does not require the private CRT.
 *
 * Typical lifetime:
 *
 *     pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
 *     ptr  = Mempool_Alloc(pool, size);
 *     Mempool_Free(ptr);
 *     Mempool_DestroyPool(pool);
 *
 * Destroy may overlap operations that have already entered the pool; it closes
 * admission and waits for those operations to leave.  Callers must stop
 * initiating work once teardown is requested and must not use the handle after
 * Destroy returns.  The kernel implementation uses a non-recursive DPC spin
 * lock for non-paged pools and retains PushLock for paged pools below DPC.
 * Paged pools are rejected at DISPATCH_LEVEL; non-paged pools may be used
 * there only with resident caller buffers.
 */

#ifndef MEMPOOL_H
#define MEMPOOL_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _KERNEL_MODE
/*
 * The pool may be either paged or non-paged.  Non-paged pools use a spin lock
 * and are legal through DISPATCH_LEVEL.  A paged pool returns NULL/no-ops at
 * DISPATCH_LEVEL instead of attempting a pageable allocation or dereference.
 */
#define MEMPOOL_IRQL_MAX_DISPATCH _IRQL_requires_max_(DISPATCH_LEVEL)
#else
#define MEMPOOL_IRQL_MAX_DISPATCH
#endif

typedef struct MEMPOOL MEMPOOL;

typedef enum MEMPOOL_TYPE {
    /* In kernel mode, backing pages may be paged out at PASSIVE_LEVEL. */
    MEMPOOL_PAGED = 0,
    /* In kernel mode, backing pages remain resident. */
    MEMPOOL_NONPAGED = 1
} MEMPOOL_TYPE;

/*
 * Creates a paged or non-paged pool, depending on type.  Returns NULL when
 * the type is invalid, a paged pool is requested at DISPATCH_LEVEL, or the
 * first backing page cannot be allocated.
 */
MEMPOOL_IRQL_MAX_DISPATCH MEMPOOL *Mempool_CreatePool(MEMPOOL_TYPE type);

/*
 * Destroys the pool and releases all pages and outstanding large chunks.
 * The return value is the number of backing pages released, including pages
 * occupied by large chunks.  All pointers from this pool become invalid.
 */
MEMPOOL_IRQL_MAX_DISPATCH ULONG Mempool_DestroyPool(MEMPOOL *pool);

/*
 * Allocates size bytes.  A zero-size request returns NULL.  The returned
 * memory is naturally aligned for the target: 8-byte on 32-bit builds and
 * 16-byte on 64-bit builds.  It must be released with Mempool_Free rather
 * than a CRT or OS free routine.
 */
MEMPOOL_IRQL_MAX_DISPATCH void *Mempool_Alloc(MEMPOOL *pool, ULONG size);

/*
 * Releases a pointer returned by Mempool_Alloc; NULL is treated as misuse.
 * Like all allocators, this routine cannot make a pointer safe after its pool
 * has been destroyed; callers must finish/clear outstanding pointers first.
 * A pointer belonging to a paged pool is not released at DISPATCH_LEVEL;
 * defer that release to PASSIVE_LEVEL or APC_LEVEL.
 */
MEMPOOL_IRQL_MAX_DISPATCH void Mempool_Free(void *ptr);

/*
 * Security/performance policy (compile-time): the implementation leaves a
 * reused cell uninitialized by default, matching the original fast path.
 * Build mempool.c with MEMPOOL_ZERO_ON_ALLOC=1 when allocations can cross a
 * trust boundary; successful allocations then clear the complete rounded
 * cell/chunk capacity before returning it, preventing stale tail bytes from a
 * previous larger allocation.  This does not change the public call sequence.
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#undef MEMPOOL_IRQL_MAX_DISPATCH

#endif /* MEMPOOL_H */
