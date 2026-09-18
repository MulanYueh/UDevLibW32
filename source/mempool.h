/*
 * Windows user-mode/kernel-mode memory pool.
 *
 * The public API is intentionally small.  All implementation details,
 * including the page lists and synchronization object, stay private to
 * mempool.c.
 *
 * Both user-mode and kernel-mode builds link libc.c from the team's private
 * CRT for algorithmic byte operations (libc_memset/libc_memcpy).  Kernel
 * backing allocations route through allocator.c.  Its default backend keeps
 * Windows 10 version 1809 compatibility; newer-target builds may opt in to
 * ExAllocatePool2 at compile time.
 *
 * Typical lifetime:
 *
 *     pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
 *     ptr  = Mempool_Alloc(pool, size);
 *     Mempool_Free(ptr);
 *     Mempool_DestroyPool(pool);
 *
 * Destroy may overlap operations that have already entered the pool; it closes
 * admission and waits for those operations to leave without holding the global
 * registry lock.  Callers must stop initiating work once teardown is requested
 * and must not use the handle after Destroy returns.  Kernel backing pages are
 * deliberately resident for both selectors because Mempool_Free's DPC-safe
 * address registry embeds lookup nodes in small-page headers.  MEMPOOL_PAGED
 * is therefore a compatibility/IRQL policy selector, not a pageable-backing
 * guarantee.
 *
 * Registry work is bounded for DISPATCH_LEVEL predictability and corruption
 * containment.  Each of 256 internal hash buckets admits at most 256 live pool
 * handles or backing objects (a theoretical global ceiling of 65,536 of each;
 * hash collisions can cause Create/Alloc to return NULL earlier).  This is a
 * deliberate fail-closed capacity limit, independent of mutable bookkeeping
 * counters, rather than an out-of-memory indication from the OS allocator.
 * Small-page selection uses a fixed array of exact maximum-free-run bins, so
 * allocation work does not grow with the number of pages while holding the
 * pool lock.  Its compact counters cap one pool at 65,535 small pages; reaching
 * that limit makes a further small-page growth request return NULL.
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

/* Keep the public ABI independent of a consuming project's /Gd or /Gz
 * setting.  This definition is shared with the other memory-library headers. */
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

#ifdef _KERNEL_MODE
/*
 * Both selectors use resident backing.  MEMPOOL_NONPAGED is legal through
 * DISPATCH_LEVEL; the compatibility MEMPOOL_PAGED selector is limited to APC.
 */
#define MEMPOOL_IRQL_MAX_DISPATCH _IRQL_requires_max_(DISPATCH_LEVEL)
#define MEMPOOL_IRQL_PASSIVE _IRQL_requires_(PASSIVE_LEVEL)
#else
#define MEMPOOL_IRQL_MAX_DISPATCH
#define MEMPOOL_IRQL_PASSIVE
#endif

typedef struct MEMPOOL MEMPOOL;

/* When MEMPOOL_ZERO_ON_ALLOC is enabled in a kernel build, cap the amount of
 * synchronous clearing accepted at DISPATCH_LEVEL.  Set this compile-time
 * value to zero to disable the cap.  PASSIVE_LEVEL/APC_LEVEL are unaffected. */
#ifndef MEMPOOL_MAX_DPC_ZERO_BYTES
#define MEMPOOL_MAX_DPC_ZERO_BYTES (64UL * 1024UL)
#endif

#if defined(MEMPOOL_TESTING)
/* Test-only access to the otherwise private registration index.  Production
 * builds neither declare nor emit these fault-injection hooks. */
typedef struct MEMPOOL_REGISTRY_TEST_SNAPSHOT {
    void *registration;
    void *registration_next;
    ULONG registration_count;
    ULONG bucket_count;
    ULONG bucket;
} MEMPOOL_REGISTRY_TEST_SNAPSHOT;

typedef enum MEMPOOL_REGISTRY_TEST_CORRUPTION {
    MEMPOOL_REGISTRY_TEST_SELF_CYCLE = 1,
    MEMPOOL_REGISTRY_TEST_GLOBAL_COUNT_SMALL = 2,
    MEMPOOL_REGISTRY_TEST_GLOBAL_COUNT_LARGE = 3,
    MEMPOOL_REGISTRY_TEST_BUCKET_COUNT_SMALL = 4,
    MEMPOOL_REGISTRY_TEST_BUCKET_COUNT_LARGE = 5
} MEMPOOL_REGISTRY_TEST_CORRUPTION;

int DEVLIB_API_CALL mempool_test_registry_snapshot(
    MEMPOOL *pool,
    MEMPOOL_REGISTRY_TEST_SNAPSHOT *snapshot);
int DEVLIB_API_CALL mempool_test_registry_corrupt(
    const MEMPOOL_REGISTRY_TEST_SNAPSHOT *snapshot,
    MEMPOOL_REGISTRY_TEST_CORRUPTION corruption);
void DEVLIB_API_CALL mempool_test_registry_restore(
    const MEMPOOL_REGISTRY_TEST_SNAPSHOT *snapshot);
int DEVLIB_API_CALL mempool_test_dpc_zero_request_allowed(ULONG size);
#endif

typedef enum MEMPOOL_TYPE {
    /* Compatibility selector: operations are rejected at DISPATCH_LEVEL, but
       backing remains resident.  Use a different allocator when actual paged
       backing is required. */
    MEMPOOL_PAGED = 0,
    /* In kernel mode, backing pages remain resident. */
    MEMPOOL_NONPAGED = 1
} MEMPOOL_TYPE;

/*
 * Creates a pool with the scheduling policy selected by type.  Returns NULL
 * when the type is invalid, the call is made above PASSIVE_LEVEL in kernel
 * mode, the first backing page cannot be allocated, or the bounded live-handle
 * registry cannot admit another entry.
 */
MEMPOOL_IRQL_PASSIVE MEMPOOL *DEVLIB_API_CALL
Mempool_CreatePool(MEMPOOL_TYPE type);

/*
 * Destroys the pool and releases all pages and outstanding large chunks.
 * The return value is the number of backing pages released, including pages
 * occupied by large chunks.  All pointers from this pool become invalid.
 * If internal ownership/index metadata is found corrupt, teardown quarantines
 * the pool instead: the handle remains permanently closed and unregistered,
 * no backing is released, and user-mode builds return zero if execution is
 * continued after DebugBreak.  The quarantined storage is intentionally leaked
 * rather than risking a partial unlink, use-after-free, or unrelated free.
 * Kernel creation and teardown are restricted to PASSIVE_LEVEL because they
 * allocate metadata and teardown waits for this pool's in-flight operations.
 */
MEMPOOL_IRQL_PASSIVE ULONG DEVLIB_API_CALL Mempool_DestroyPool(MEMPOOL *pool);

/*
 * Allocates size bytes.  A zero-size request returns NULL.  The returned
 * memory is naturally aligned for the target: 8-byte on 32-bit builds and
 * 16-byte on 64-bit builds.  It must be released with Mempool_Free rather
 * than a CRT or OS free routine.  NULL can also mean that a bounded backing
 * registry or per-pool page index cannot admit the page/chunk even when its
 * raw OS allocation succeeded; that raw allocation is rolled back before
 * returning.
 */
MEMPOOL_IRQL_MAX_DISPATCH void *DEVLIB_API_CALL
Mempool_Alloc(MEMPOOL *pool, ULONG size);

/*
 * Releases a pointer returned by Mempool_Alloc; NULL is treated as misuse.
 * Like all allocators, this routine cannot make a pointer safe after its pool
 * has been destroyed; callers must finish/clear outstanding pointers first.
 * A pointer belonging to a paged pool is not released at DISPATCH_LEVEL;
 * defer that release to PASSIVE_LEVEL or APC_LEVEL.  This restriction remains
 * part of the compatibility contract even though backing is resident.
 */
MEMPOOL_IRQL_MAX_DISPATCH void DEVLIB_API_CALL Mempool_Free(void *ptr);

/*
 * Security/performance policy (compile-time): the implementation leaves a
 * reused cell uninitialized by default, matching the original fast path.
 * Build mempool.c with MEMPOOL_ZERO_ON_ALLOC=1 when allocations can cross a
 * trust boundary; successful allocations then clear the complete rounded
 * cell/chunk capacity before returning it, preventing stale tail bytes from a
 * previous larger allocation.  In a kernel build, DISPATCH_LEVEL requests
 * whose rounded clearing work exceeds MEMPOOL_MAX_DPC_ZERO_BYTES return NULL
 * before allocating or publishing backing.  Define that limit as zero to
 * disable it.  Lower IRQLs are unaffected.  This does not change the public
 * call sequence.
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#undef MEMPOOL_IRQL_MAX_DISPATCH
#undef MEMPOOL_IRQL_PASSIVE

#endif /* MEMPOOL_H */
