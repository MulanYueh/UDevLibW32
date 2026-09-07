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
 * Destroy returns.  The kernel implementation uses a non-recursive push lock and
 * enters a critical region while holding it, so callers must also avoid
 * recursively entering the same pool operation.  All public functions are
 * annotated for APC_LEVEL or below because acquisition may wait and paged
 * pools may fault.
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
 * The pool may be either paged or non-paged, and its PushLock can wait.  A
 * single APC_LEVEL ceiling is therefore the safe contract for every public
 * operation.  Callers that need DISPATCH_LEVEL allocation must use a
 * dedicated non-blocking allocator instead of this pool.
 */
#define MEMPOOL_IRQL_MAX_APC _IRQL_requires_max_(APC_LEVEL)
#else
#define MEMPOOL_IRQL_MAX_APC
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
 * the type is invalid or the first backing page cannot be allocated.
 */
MEMPOOL_IRQL_MAX_APC MEMPOOL *Mempool_CreatePool(MEMPOOL_TYPE type);

/*
 * Destroys the pool and releases all pages and outstanding large chunks.
 * The return value is the number of backing pages released, including pages
 * occupied by large chunks.  All pointers from this pool become invalid.
 */
MEMPOOL_IRQL_MAX_APC ULONG Mempool_DestroyPool(MEMPOOL *pool);

/*
 * Allocates size bytes.  A zero-size request returns NULL.  The returned
 * memory is naturally aligned for the target: 8-byte on 32-bit builds and
 * 16-byte on 64-bit builds.  It must be released with Mempool_Free rather
 * than a CRT or OS free routine.
 */
MEMPOOL_IRQL_MAX_APC void *Mempool_Alloc(MEMPOOL *pool, ULONG size);

/*
 * Releases a pointer returned by Mempool_Alloc; NULL is treated as misuse.
 * Like all allocators, this routine cannot make a pointer safe after its pool
 * has been destroyed; callers must finish/clear outstanding pointers first.
 */
MEMPOOL_IRQL_MAX_APC void Mempool_Free(void *ptr);

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

#undef MEMPOOL_IRQL_MAX_APC

#endif /* MEMPOOL_H */
