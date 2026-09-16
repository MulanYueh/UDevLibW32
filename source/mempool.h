/*
 * Windows user-mode/kernel-mode memory pool.
 *
 * The public API is intentionally small.  All implementation details,
 * including the page lists and synchronization object, stay private to
 * mempool.c.
 *
 * Both user-mode and kernel-mode builds link libc.c from the team's private
 * CRT for algorithmic byte operations (libc_memset/libc_memcpy).  Kernel
 * backing allocations route through allocator.c, which selects ExAllocatePool2
 * or the legacy pool API according to the driver's target WDK version.
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
 * Destroy returns.  The kernel implementation uses one non-recursive DPC
 * spin lock for every pool.  Kernel backing pages are resident because the
 * lock raises IRQL; the logical paged pool is still rejected at DISPATCH_LEVEL
 * to preserve its allocation contract.
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
#define MEMPOOL_IRQL_PASSIVE _IRQL_requires_(PASSIVE_LEVEL)
#else
#define MEMPOOL_IRQL_MAX_DISPATCH
#define MEMPOOL_IRQL_PASSIVE
#endif

typedef struct MEMPOOL MEMPOOL;

typedef enum MEMPOOL_TYPE {
    /* Logical paged pool; kernel backing pages remain resident for spin-lock
       metadata safety and this type is rejected at DISPATCH_LEVEL. */
    MEMPOOL_PAGED = 0,
    /* In kernel mode, backing pages remain resident. */
    MEMPOOL_NONPAGED = 1
} MEMPOOL_TYPE;

/*
 * Creates a paged or non-paged pool, depending on type.  Returns NULL when
 * the type is invalid, the call is made above PASSIVE_LEVEL in kernel mode,
 * or the first backing page cannot be allocated.
 */
MEMPOOL_IRQL_PASSIVE MEMPOOL *Mempool_CreatePool(MEMPOOL_TYPE type);

/*
 * Destroys the pool and releases all pages and outstanding large chunks.
 * The return value is the number of backing pages released, including pages
 * occupied by large chunks.  All pointers from this pool become invalid.
 * Kernel creation and teardown are restricted to PASSIVE_LEVEL because they
 * take a process-wide writer gate and wait for in-flight operations.  Use
 * PASSIVE_LEVEL when the pool is part of a manager that also owns paged state.
 */
MEMPOOL_IRQL_PASSIVE ULONG Mempool_DestroyPool(MEMPOOL *pool);

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
 * defer that release to PASSIVE_LEVEL or APC_LEVEL.  Because this API receives
 * only the allocation pointer, the caller must uphold that residency contract
 * before the implementation reads its hidden owner word.
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
#undef MEMPOOL_IRQL_PASSIVE

#endif /* MEMPOOL_H */
