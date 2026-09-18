/*
 * Windows user-mode/kernel-mode memory pool.
 *
 * Small allocations are served from page-local fixed-size cells tracked by a
 * bitmap.  Allocations near a page or larger are page-aligned large chunks;
 * their descriptors live in separate resident metadata.  A pool owns all
 * pages and large chunks and releases them from Mempool_DestroyPool.
 *
 * Coding rule: every function-local object is initialized at its declaration.
 * Keep declarations at the beginning of each function/block for old MSVC.
 */

#include "mempool.h"
#include "def.h"
#include "allocator.h"

/*
 * The project CRT is freestanding and is also available to user-mode
 * and kernel-mode callers.  Keep this source's dependency narrow instead of
 * including the full libc.h: these declarations mirror the raw-memory ABI in
 * libc.h and avoid pulling optional declarations into old MSVC builds.  The
 * kernel image must link the resident libc_memset/libc_memcpy objects.
 */
#include <stddef.h>
#if defined(_MSC_VER)
#define MEMPOOL_LIBC_CALL __cdecl
#elif defined(__i386__) && defined(_WIN32) && \
      (defined(__GNUC__) || defined(__clang__))
#define MEMPOOL_LIBC_CALL __attribute__((__cdecl__))
#else
#define MEMPOOL_LIBC_CALL
#endif
extern void *MEMPOOL_LIBC_CALL libc_memset(void *dst, int value,
                                           size_t count);
extern void *MEMPOOL_LIBC_CALL libc_memcpy(void *dst, const void *src,
                                           size_t count);
#undef MEMPOOL_LIBC_CALL

/*
 * Design overview
 * ----------------
 *
 * The allocator has two paths.  Small requests are rounded to fixed-size
 * cells and served from a page bitmap.  Large requests are rounded to whole
 * pages and tracked by independent side descriptors.  This avoids asking the OS for
 * every small object while keeping large objects out of the bitmap search.
 *
 * A normal page looks like this (one bitmap bit describes one cell):
 *
 *   page base
 *       | page header | bitmap | length table | cell 0 | ... | cell N-1 |
 *       +-------------+--------+--------------+--------+-----+----------+
 *                                             ^
 *                              internal cell start (size word at +0,
 *                              caller pointer after the hidden aligned header)
 *
 * The first page is special: the POOL object is placed where cell 0 starts.
 * Its cells are marked used in the bitmap, so they can never be returned to
 * a caller:
 *
 *   page base
 *       | header | bitmap | lengths | POOL + initial_bitmap | free ... |
 *       +--------+--------+---------+-----------------------+----------+
 *                                      ^
 *                             pool pointer
 *
 * The independent per-page length table records only allocation starts and
 * their exact cell spans, so a corrupt hidden size cannot clear an adjacent
 * allocation.  Every allocation stores its total internal size (requested
 * bytes plus the hidden aligned header), an allocation cookie, and the owner.
 * Mempool_Free validates that owner only after the backing registry has pinned
 * independent lifetime metadata.  Small-page registry nodes are embedded in
 * their resident page headers, so MEMPOOL_PAGED calls remain rejected at DPC.
 *
 * Page ownership is represented by one intrusive list.  Allocation never
 * walks that list: each page is also linked into the exact max-free-run bin
 * that describes its bitmap.  A separate hash registry maps every aligned
 * backing base to resident metadata so Free can pin the owning registration
 * before it reads caller-adjacent memory:
 *
 *     pool->pages      -> [page] -> [page] -> NULL   (ownership only)
 *     pool->run_bins[n] -> [page with max run n] -> NULL
 *     pool->large_chunks -> [chunk] -> [chunk] -> NULL
 */

typedef struct MEMPOOL_LIST_ELEM MEMPOOL_LIST_ELEM;
typedef struct MEMPOOL_LIST MEMPOOL_LIST;

/*
 * Lists are intrusive: the first bytes of PAGE and LARGE_CHUNK are the list
 * node itself.  LARGE_CHUNK is independent side metadata, while a page embeds
 * both its pool-list node and its global-backing-registry node.
 */
struct MEMPOOL_LIST_ELEM {
    MEMPOOL_LIST_ELEM *next;
    MEMPOOL_LIST_ELEM *prev;
};

struct MEMPOOL_LIST {
    MEMPOOL_LIST_ELEM *head;
    MEMPOOL_LIST_ELEM *tail;
    int count;
};

/* Short names keep the allocator code readable; these aliases are private. */
#define LIST_ELEM MEMPOOL_LIST_ELEM
#define LIST MEMPOOL_LIST
#define POOL MEMPOOL
#define List_Init(list) \
    ((list)->head = (LIST_ELEM *)0, (list)->tail = (LIST_ELEM *)0, \
     (list)->count = 0)

#ifdef _KERNEL_MODE
/* Kernel pool allocations are aligned to the system page size. */
#define MEMPOOL_PAGE_SIZE ((ULONG)PAGE_SIZE)
#define MEMPOOL_CELL_SIZE 16UL
#else
/* VirtualAlloc reserves address space at 64 KiB granularity. */
#define MEMPOOL_PAGE_SIZE 65536UL
#define MEMPOOL_CELL_SIZE 128UL
#endif

#ifndef MEMPOOL_LARGE_CHUNK_MINIMUM
/* Keep near-page requests out of the cell bitmap. */
#define MEMPOOL_LARGE_CHUNK_MINIMUM (MEMPOOL_PAGE_SIZE * 3UL / 4UL)
#endif

/*
 * A zero value preserves the original fast, uninitialized allocation path.
 * Define MEMPOOL_ZERO_ON_ALLOC=1 for pools that may carry sensitive data to a
 * less-trusted caller; this is a build-time policy so the public API stays
 * minimal.  It also clears cells reused after Free, not only fresh OS pages.
 */
#ifndef MEMPOOL_ZERO_ON_ALLOC
#define MEMPOOL_ZERO_ON_ALLOC 0
#endif

/* Cell sizes are powers of two, which makes the padding and address math cheap. */
#define MEMPOOL_PAD_CELL(value) \
    (((value) + MEMPOOL_CELL_SIZE - 1UL) & ~(MEMPOOL_CELL_SIZE - 1UL))
#define MEMPOOL_ALLOCATION_ALIGNMENT ((SIZE_T)(sizeof(ULONG_PTR) * 2UL))
#define MEMPOOL_PAD_ALIGNMENT(value) \
    (((SIZE_T)(value) + MEMPOOL_ALLOCATION_ALIGNMENT - (SIZE_T)1) & \
     ~(MEMPOOL_ALLOCATION_ALIGNMENT - (SIZE_T)1))
#define MEMPOOL_NUM_CELLS(value) \
    (MEMPOOL_PAD_CELL(value) / MEMPOOL_CELL_SIZE)
#define MEMPOOL_PAGE_MASK ((ULONG_PTR)MEMPOOL_PAGE_SIZE - (ULONG_PTR)1)

typedef struct MEMPOOL_PAGE MEMPOOL_PAGE;
typedef struct MEMPOOL_LARGE_CHUNK MEMPOOL_LARGE_CHUNK;
typedef struct MEMPOOL_ALLOCATION_HEADER MEMPOOL_ALLOCATION_HEADER;
typedef struct MEMPOOL_REGISTRATION MEMPOOL_REGISTRATION;
typedef struct MEMPOOL_BACKING MEMPOOL_BACKING;
#ifdef _KERNEL_MODE
/* Kernel small allocations use fewer than 256 cells, so one byte is exact. */
typedef UCHAR MEMPOOL_CELL_COUNT_ENTRY;
#define MEMPOOL_CELL_COUNT_ENTRY_MAX 0xFFUL
#else
typedef USHORT MEMPOOL_CELL_COUNT_ENTRY;
#define MEMPOOL_CELL_COUNT_ENTRY_MAX 0xFFFFUL
#endif

struct MEMPOOL_ALLOCATION_HEADER {
    ULONG total_size;
    ULONG cookie;
    POOL *pool;
};

enum {
    MEMPOOL_BACKING_PAGE = 1,
    MEMPOOL_BACKING_LARGE = 2
};

struct MEMPOOL_BACKING {
    MEMPOOL_BACKING *hash_next;
    MEMPOOL_BACKING *identity_next;
    MEMPOOL_BACKING *owner_next;
    MEMPOOL_BACKING *owner_prev;
    void *base;
    void *object;
    MEMPOOL_REGISTRATION *registration;
    ULONG size;
    ULONG tag;
    ULONG kind;
    volatile LONG references;
    volatile LONG closing;
};

#define MEMPOOL_ALLOCATION_HEADER_SIZE \
    MEMPOOL_PAD_ALIGNMENT(sizeof(MEMPOOL_ALLOCATION_HEADER))

struct MEMPOOL_PAGE {
    LIST_ELEM list_elem; /* Intrusive node for the ownership page list. */
    MEMPOOL_BACKING backing; /* Global lookup node; embedded and resident. */
    MEMPOOL_PAGE *bin_next; /* Independent exact-max-run bin links. */
    MEMPOOL_PAGE *bin_prev;
    MEMPOOL_PAGE *next;  /* Reserved for compatibility; list_elem is used. */
    POOL *pool;          /* Owning pool, recovered by Mempool_Free. */
    ULONG eyecatcher;    /* Pool tag used for corruption checks and freeing. */
    USHORT num_free;     /* Exact free-cell count used as a search filter. */
    USHORT num_used;     /* Exact number of allocated cells for reclamation. */
    USHORT max_free_run; /* Exact bitmap run and run-bin index. */
};

#define MEMPOOL_PAGE_HEADER_SIZE \
    MEMPOOL_PAD_CELL(sizeof(MEMPOOL_PAGE))
/*
 * One bitmap bit represents one potential cell.  The bitmap is itself
 * cell-padded so the following data region starts on a cell boundary.
 */
#define MEMPOOL_PAGE_BITMAP_SIZE \
    MEMPOOL_PAD_CELL((((MEMPOOL_PAGE_SIZE - MEMPOOL_PAGE_HEADER_SIZE) / \
                       MEMPOOL_CELL_SIZE) + 7UL) / 8UL)
#define MEMPOOL_PAGE_LENGTHS_SIZE \
    MEMPOOL_PAD_CELL(((MEMPOOL_PAGE_SIZE - MEMPOOL_PAGE_HEADER_SIZE) / \
                      MEMPOOL_CELL_SIZE) * \
                     sizeof(MEMPOOL_CELL_COUNT_ENTRY))
#define MEMPOOL_PAGE_DATA_OFFSET \
    (MEMPOOL_PAGE_HEADER_SIZE + MEMPOOL_PAGE_BITMAP_SIZE + \
     MEMPOOL_PAGE_LENGTHS_SIZE)
#define MEMPOOL_NUM_PAGE_CELLS \
    ((MEMPOOL_PAGE_SIZE - MEMPOOL_PAGE_DATA_OFFSET) / MEMPOOL_CELL_SIZE)
#define MEMPOOL_RUN_BIN_COUNT (MEMPOOL_NUM_PAGE_CELLS + 1UL)
#define MEMPOOL_PAGE_COUNT_LIMIT 0xFFFFUL

struct POOL {
    ULONG eyecatcher; /* Same tag as all pages owned by this pool. */
    ULONG pool_type;  /* MEMPOOL_PAGED or MEMPOOL_NONPAGED. */
    MEMPOOL_REGISTRATION *registration; /* Resident lifetime record. */
#ifdef _KERNEL_MODE
    /* One DPC-safe spin lock protects every kernel pool instance. */
    KSPIN_LOCK spin_lock;
#else
    CRITICAL_SECTION lock; /* Critical sections are recursive in user mode. */
#endif
    ULONG cookie_seed; /* Per-pool salt for hidden allocation headers. */
    LIST pages;       /* Ownership only; allocation never traverses it. */
    LIST full_pages;  /* Retained empty for private-layout compatibility. */
    LIST large_chunks; /* Independently allocated large-chunk descriptors. */
    ULONG large_chunk_count;
    ULONG large_chunk_count_check;
    MEMPOOL_PAGE *run_bins[MEMPOOL_RUN_BIN_COUNT];
    USHORT run_bin_counts[MEMPOOL_RUN_BIN_COUNT];
    USHORT run_bin_checks[MEMPOOL_RUN_BIN_COUNT];
    ULONG page_count;       /* Exact ownership-list count. */
    ULONG page_count_check; /* Modulo-2^32 negation of page_count. */
    UCHAR initial_bitmap[MEMPOOL_PAGE_BITMAP_SIZE]; /* Padding mask for new pages. */
};

/* The owner object lives inside the first page's data cells.  Keep this a
 * compile-time property for every pointer width and kernel/user page layout. */
typedef char MEMPOOL_POOL_OBJECT_MUST_FIT_IN_PAGE[
    MEMPOOL_NUM_CELLS(sizeof(POOL)) <= MEMPOOL_NUM_PAGE_CELLS ? 1 : -1];
typedef char MEMPOOL_PAGE_COUNT_MUST_FIT_METADATA[
    MEMPOOL_NUM_PAGE_CELLS <= MEMPOOL_CELL_COUNT_ENTRY_MAX ? 1 : -1];
typedef char MEMPOOL_RUN_BIN_INDEX_MUST_FIT_USHORT[
    MEMPOOL_NUM_PAGE_CELLS <= 0xFFFFUL ? 1 : -1];

struct MEMPOOL_LARGE_CHUNK {
    LIST_ELEM list_elem; /* Intrusive node for pool->large_chunks. */
    MEMPOOL_BACKING backing; /* Side metadata used by pointer-first lookup. */
    ULONG eyecatcher;    /* Tag copied from the owning pool. */
    POOL *pool;          /* Owner used to find the correct list and lock. */
    void *ptr;           /* Page-aligned OS allocation base. */
    ULONG size;          /* Entire OS allocation, including padding. */
    ULONG allocation_size; /* Exact internal allocation size. */
    ULONG cookie;        /* Independent copy of the header cookie. */
};

/*
 * The page-rounding expression adds almost one full page before masking.
 * Keep enough headroom that this addition cannot wrap
 * a 32-bit ULONG.  This is intentionally a little conservative, matching
 * the original allocator's safe upper bound.
 */
#define MEMPOOL_LARGE_CHUNK_MAXIMUM \
    ((ULONG)0xFFFFFFFFUL - (MEMPOOL_PAGE_SIZE - 1UL))
#define MEMPOOL_MAX_ALLOC_SIZE \
    (MEMPOOL_LARGE_CHUNK_MAXIMUM - (ULONG)MEMPOOL_ALLOCATION_HEADER_SIZE)

enum {
    MEMPOOL_ABEND_FREE_SIZE_MISMATCH = 1, /* Invalid/overflowed size word. */
    MEMPOOL_ABEND_FREE_NULL = 2,          /* NULL is not a valid allocation. */
    MEMPOOL_ABEND_LARGE_HEADER = 3,       /* Large-chunk metadata is corrupt. */
    MEMPOOL_ABEND_GET_CELLS_HEADER = 4,   /* A page tag does not match its pool. */
    MEMPOOL_ABEND_FREE_CELLS_HEADER = 5,  /* Cell page has no valid owner. */
    MEMPOOL_ABEND_FREE_CELLS_RANGE = 6,   /* Address/index is outside the page. */
    MEMPOOL_ABEND_FREE_CELLS_DOUBLE = 7,  /* At least one cell was already free. */
    MEMPOOL_ABEND_FREE_CELLS_COUNT = 8,   /* Exact used-cell counter is corrupt. */
    MEMPOOL_ABEND_ALLOCATION_METADATA = 9 /* Cookie/start/length mismatch. */
};

/* The high bit closes admission; the remaining bits count active operations. */
#define MEMPOOL_LIFECYCLE_CLOSING ((LONG)0x80000000L)
#define MEMPOOL_LIFECYCLE_COUNT_MASK ((LONG)0x7FFFFFFFL)

struct MEMPOOL_REGISTRATION {
    POOL *pool;
    ULONG pool_type;
    volatile LONG lifecycle_state;
    MEMPOOL_BACKING *backings;
    ULONG backing_count;
    MEMPOOL_REGISTRATION *next;
};
#define MEMPOOL_REGISTRY_BUCKET_COUNT 256UL
#define MEMPOOL_REGISTRY_BUCKET_MASK (MEMPOOL_REGISTRY_BUCKET_COUNT - 1UL)
/* A compile-time bound is deliberately independent of mutable registry
 * counters.  Every spin-lock walk is capped by this value, including DPC-level
 * lookup/publication/retirement. */
#define MEMPOOL_REGISTRY_BUCKET_LIMIT 256UL
#define MEMPOOL_REGISTRY_CAPACITY \
    (MEMPOOL_REGISTRY_BUCKET_COUNT * MEMPOOL_REGISTRY_BUCKET_LIMIT)
static MEMPOOL_REGISTRATION *
    mempool_registry[MEMPOOL_REGISTRY_BUCKET_COUNT];
static MEMPOOL_BACKING *
    mempool_backing_registry[MEMPOOL_REGISTRY_BUCKET_COUNT];
static MEMPOOL_BACKING *
    mempool_backing_identity_registry[MEMPOOL_REGISTRY_BUCKET_COUNT];
/* Exact while mempool_registry_lock is held.  Each primary count has a
 * separately updated modulo-2^32 negation so one damaged count cannot silently
 * become a traversal bound. */
static ULONG
    mempool_registration_bucket_counts[MEMPOOL_REGISTRY_BUCKET_COUNT];
static ULONG
    mempool_registration_bucket_checks[MEMPOOL_REGISTRY_BUCKET_COUNT];
static ULONG
    mempool_backing_base_bucket_counts[MEMPOOL_REGISTRY_BUCKET_COUNT];
static ULONG
    mempool_backing_base_bucket_checks[MEMPOOL_REGISTRY_BUCKET_COUNT];
static ULONG
    mempool_backing_identity_bucket_counts[MEMPOOL_REGISTRY_BUCKET_COUNT];
static ULONG
    mempool_backing_identity_bucket_checks[MEMPOOL_REGISTRY_BUCKET_COUNT];
static ULONG mempool_registration_count = 0;
static ULONG mempool_registration_count_check = 0;
static ULONG mempool_backing_registry_count = 0;
static ULONG mempool_backing_registry_count_check = 0;
#ifdef _KERNEL_MODE
static KSPIN_LOCK mempool_registry_lock;
static volatile LONG mempool_registry_lock_initialized = 0;
#else
static SRWLOCK mempool_registry_lock = SRWLOCK_INIT;
#endif

static void mempool_zero(void *address, SIZE_T length);
static void mempool_copy(void *destination, const void *source, SIZE_T length);
static void *mempool_alloc_mem(ULONG pool_type, ULONG size, ULONG tag);
static void mempool_free_mem(void *address, ULONG tag);
static void *mempool_metadata_alloc(SIZE_T size);
static void mempool_metadata_free(void *address);
static MEMPOOL_PAGE *mempool_alloc_page(POOL *pool, ULONG pool_type, ULONG tag);
static ULONG mempool_find_cells(MEMPOOL_PAGE *page, ULONG cell_count,
                                ULONG *largest_run, ULONG *free_cells);
static void *mempool_get_cells(POOL *pool, ULONG cell_count);
static void mempool_free_cells(MEMPOOL_PAGE *page, void *address,
                               ULONG total_size);
static void *mempool_get_large_chunk(POOL *pool, ULONG size);
static void mempool_free_large_chunk(MEMPOOL_LARGE_CHUNK *chunk,
                                     void *address, ULONG size);
static POOL *mempool_create(ULONG pool_type, ULONG tag);
static void mempool_abend(ULONG reason);
static ULONG mempool_allocation_cookie(POOL *pool, void *address,
                                       ULONG total_size);
#if MEMPOOL_ZERO_ON_ALLOC || defined(MEMPOOL_TESTING)
static SIZE_T mempool_zero_clear_size(ULONG size);
static int mempool_dpc_zero_request_allowed(ULONG size);
#endif
static int mempool_operation_enter(MEMPOOL_REGISTRATION *registration);
static void mempool_operation_leave(MEMPOOL_REGISTRATION *registration);
static int mempool_begin_destroy(MEMPOOL_REGISTRATION *registration);

/*
 * Kernel backing pages (including the logical paged-pool variant) are resident
 * because the pool lock may raise IRQL while metadata is inspected.  A
 * logical paged pool is still rejected at DPC before any list/bitmap access;
 * its public contract remains PASSIVE/APC-only.  The registry/admission gate
 * is acquired by public callers before this helper dereferences the pool.
 */
static int mempool_current_irql_allows_pool(POOL *pool)
{
#ifdef _KERNEL_MODE
    KIRQL current_irql = PASSIVE_LEVEL;
#endif

    if (pool == (POOL *)0)
        return 0;
#ifdef _KERNEL_MODE
    current_irql = KeGetCurrentIrql();
    if (current_irql > DISPATCH_LEVEL)
        return 0;
    if (current_irql == DISPATCH_LEVEL &&
        pool->pool_type == (ULONG)MEMPOOL_PAGED)
        return 0;
#endif
    return 1;
}

#if MEMPOOL_ZERO_ON_ALLOC || defined(MEMPOOL_TESTING)
/* Return the exact payload capacity cleared by Mempool_Alloc.  Keeping this
 * calculation shared with the DPC admission check prevents a future layout
 * change from making the limit underestimate the synchronous work. */
static SIZE_T mempool_zero_clear_size(ULONG size)
{
    ULONG total_size = 0;
    ULONG cell_count = 0;
    ULONG rounded_size = 0;

    if (size == 0 || size > MEMPOOL_MAX_ALLOC_SIZE)
        return (SIZE_T)0;
    total_size = size + (ULONG)MEMPOOL_ALLOCATION_HEADER_SIZE;
    if (total_size > MEMPOOL_LARGE_CHUNK_MINIMUM) {
        rounded_size = (total_size + MEMPOOL_PAGE_SIZE - 1UL) &
                       ~(MEMPOOL_PAGE_SIZE - 1UL);
        return (SIZE_T)rounded_size -
               (SIZE_T)MEMPOOL_ALLOCATION_HEADER_SIZE;
    }
    cell_count = MEMPOOL_NUM_CELLS(total_size);
    return (SIZE_T)cell_count * (SIZE_T)MEMPOOL_CELL_SIZE -
           (SIZE_T)MEMPOOL_ALLOCATION_HEADER_SIZE;
}

static int mempool_dpc_zero_request_allowed(ULONG size)
{
#if MEMPOOL_MAX_DPC_ZERO_BYTES != 0
    SIZE_T clear_size = 0;
#endif

    if (size == 0 || size > MEMPOOL_MAX_ALLOC_SIZE)
        return 0;
#if MEMPOOL_MAX_DPC_ZERO_BYTES == 0
    return 1;
#else
    clear_size = mempool_zero_clear_size(size);
    return (ULONGLONG)clear_size <=
           (ULONGLONG)(MEMPOOL_MAX_DPC_ZERO_BYTES);
#endif
}
#endif

static void mempool_zero(void *address, SIZE_T length)
{
    /* libc_memset is the project's CRT-independent implementation; callers
       pass storage owned by the pool and a length checked by the layout. */
    (void)libc_memset(address, 0, (size_t)length);
}

static void mempool_copy(void *destination, const void *source, SIZE_T length)
{
    /* The source and destination are non-overlapping bitmap regions.  Keep
       this primitive separate from the checked Move path, which uses memmove
       when caller ranges may overlap. */
    (void)libc_memcpy(destination, source, (size_t)length);
}

static ULONG mempool_allocation_cookie(POOL *pool, void *address,
                                       ULONG total_size)
{
    ULONG_PTR mixed = (ULONG_PTR)0;

    mixed = (ULONG_PTR)address ^ ((ULONG_PTR)pool >> 3) ^
            ((ULONG_PTR)total_size * (ULONG_PTR)0x9E3779B1UL) ^
            (ULONG_PTR)pool->cookie_seed;
#if defined(_WIN64) || defined(_M_AMD64) || defined(_M_ARM64) || \
    defined(__x86_64__) || defined(__aarch64__)
    mixed ^= mixed >> 32;
#endif
    mixed ^= mixed >> 16;
    mixed *= (ULONG_PTR)0x85EBCA6BUL;
    mixed ^= mixed >> 13;
    return (ULONG)mixed ^ (ULONG)0xC2B2AE35UL;
}

static void mempool_abend(ULONG reason)
{
    /* Corruption is a programming error: fail immediately instead of
       continuing with a potentially unrelated page or pool. */
#ifdef _KERNEL_MODE
#ifndef DRIVER_CORRUPTED_MMPOOL
#define DRIVER_CORRUPTED_MMPOOL 0x000000D9UL
#endif
    KeBugCheckEx(DRIVER_CORRUPTED_MMPOOL, (ULONG_PTR)MEMPOOL_POOL_TAG,
                 (ULONG_PTR)reason, 0, 0);
#else
    UNREFERENCED_PARAMETER(reason);
    OutputDebugStringW(L"mempool corruption\n");
    DebugBreak();
#endif
}

/* ------------------------------------------------------------------------- */
/* Platform lock selection                                                   */
/* ------------------------------------------------------------------------- */

/*
 * One exclusive lock protects both page lists and the bitmap state.  Large-
 * chunk list updates use the same lock.  Non-paged pools use a DPC-safe spin
 * lock.  Kernel backing pages are resident so page metadata and bitmap bytes
 * are safe to inspect while KeAcquireSpinLock has raised IRQL.  A logical
 * MEMPOOL_PAGED still remains unavailable to callers at DISPATCH_LEVEL; this
 * preserves the existing pool-type contract while keeping synchronization
 * uniform and DPC-safe.
 */
#ifdef _KERNEL_MODE
typedef KIRQL MEMPOOL_LOCK_IRQL;
typedef int MEMPOOL_LOCK_MODE;
#define MEMPOOL_LOCK_DECL MEMPOOL_LOCK_IRQL lock_irql = (MEMPOOL_LOCK_IRQL)0;
#define MEMPOOL_LOCK_MODE_DECL MEMPOOL_LOCK_MODE lock_mode = 0;
#define MEMPOOL_LOCK(pool) \
    do { \
        /* Use the non-raising DPC form when already at DISPATCH_LEVEL. */ \
        lock_mode = KeGetCurrentIrql() == DISPATCH_LEVEL ? 2 : 1; \
        if (lock_mode == 2) \
            KeAcquireSpinLockAtDpcLevel(&(pool)->spin_lock); \
        else \
            KeAcquireSpinLock(&(pool)->spin_lock, &lock_irql); \
    } while (0)
#define MEMPOOL_UNLOCK(pool) \
    do { \
        if (lock_mode == 2) \
            KeReleaseSpinLockFromDpcLevel(&(pool)->spin_lock); \
        else \
            KeReleaseSpinLock(&(pool)->spin_lock, lock_irql); \
    } while (0)
#else
typedef int MEMPOOL_LOCK_IRQL;
typedef int MEMPOOL_LOCK_MODE;
#define MEMPOOL_LOCK_DECL MEMPOOL_LOCK_IRQL lock_irql = (MEMPOOL_LOCK_IRQL)0;
#define MEMPOOL_LOCK_MODE_DECL
#define MEMPOOL_LOCK(pool) \
    do { (void)lock_irql; EnterCriticalSection(&(pool)->lock); } while (0)
#define MEMPOOL_UNLOCK(pool) \
    do { (void)lock_irql; LeaveCriticalSection(&(pool)->lock); } while (0)
#endif

/* ------------------------------------------------------------------------- */
/* Lifetime admission                                                       */
/* ------------------------------------------------------------------------- */

/*
 * Destroy cannot make an opaque pointer safe after it has returned; callers
 * must still stop starting new operations during teardown.  The admission
 * counters solve the important in-flight case: an operation that has entered
 * is guaranteed to finish before Destroy releases the pool object.
 *
 * Mempool_Free has no pool argument, so a fixed-bucket backing registry maps
 * the aligned address to resident metadata before any hidden header is read.
 * In kernel mode its short lock is a real KSPIN_LOCK.  Active counts live in
 * non-paged registration records, not in the pool's first page.
 *
 *   Free/Alloc: registry lock -> registration active++ -> unlock
 *   Destroy:    registry lock -> close/unlink -> unlock -> wait active==0
 */
static void mempool_passive_wait(void)
{
#ifdef _KERNEL_MODE
    LARGE_INTEGER interval = {0};

    interval.QuadPart = -10000LL; /* one millisecond, relative */
    (void)KeDelayExecutionThread(KernelMode, FALSE, &interval);
#else
    Sleep(1UL);
#endif
}

#ifdef _KERNEL_MODE
typedef KIRQL MEMPOOL_REGISTRY_IRQL;
typedef int MEMPOOL_REGISTRY_MODE;
#define MEMPOOL_REGISTRY_LOCK_DECL \
    MEMPOOL_REGISTRY_IRQL registry_irql = (MEMPOOL_REGISTRY_IRQL)0; \
    MEMPOOL_REGISTRY_MODE registry_mode = 0;

static void mempool_registry_lock_initialize(void)
{
    LONG state = 0;

    state = InterlockedCompareExchange(&mempool_registry_lock_initialized,
                                       1, 0);
    if (state == 0) {
        KeInitializeSpinLock(&mempool_registry_lock);
        (void)InterlockedExchange(&mempool_registry_lock_initialized, 2);
    } else {
        while (InterlockedCompareExchange(
                   &mempool_registry_lock_initialized, 0, 0) != 2)
            mempool_passive_wait();
    }
}

static int mempool_registry_lock_is_initialized(void)
{
    return InterlockedCompareExchange(&mempool_registry_lock_initialized,
                                      0, 0) == 2;
}

static void mempool_registry_lock_acquire(MEMPOOL_REGISTRY_IRQL *old_irql,
                                          MEMPOOL_REGISTRY_MODE *mode)
{
    if (KeGetCurrentIrql() == DISPATCH_LEVEL) {
        *mode = 2;
        KeAcquireSpinLockAtDpcLevel(&mempool_registry_lock);
    } else {
        *mode = 1;
        KeAcquireSpinLock(&mempool_registry_lock, old_irql);
    }
}

static void mempool_registry_lock_release(MEMPOOL_REGISTRY_IRQL old_irql,
                                          MEMPOOL_REGISTRY_MODE mode)
{
    if (mode == 2)
        KeReleaseSpinLockFromDpcLevel(&mempool_registry_lock);
    else
        KeReleaseSpinLock(&mempool_registry_lock, old_irql);
}
#define MEMPOOL_REGISTRY_LOCK() \
    mempool_registry_lock_acquire(&registry_irql, &registry_mode)
#define MEMPOOL_REGISTRY_UNLOCK() \
    mempool_registry_lock_release(registry_irql, registry_mode)
#else
#define MEMPOOL_REGISTRY_LOCK_DECL
static void mempool_registry_lock_initialize(void) { }
static int mempool_registry_lock_is_initialized(void) { return 1; }
#define MEMPOOL_REGISTRY_LOCK() AcquireSRWLockExclusive(&mempool_registry_lock)
#define MEMPOOL_REGISTRY_UNLOCK() ReleaseSRWLockExclusive(&mempool_registry_lock)
#endif

static int mempool_operation_enter(MEMPOOL_REGISTRATION *registration)
{
    LONG state = 0;

    if (registration == (MEMPOOL_REGISTRATION *)0)
        return 0;
    for (;;) {
        state = InterlockedCompareExchange(&registration->lifecycle_state,
                                           0, 0);
        if ((state & MEMPOOL_LIFECYCLE_CLOSING) != 0 ||
            (state & MEMPOOL_LIFECYCLE_COUNT_MASK) ==
                MEMPOOL_LIFECYCLE_COUNT_MASK)
            return 0;
        if (InterlockedCompareExchange(&registration->lifecycle_state,
                                       state + 1, state) == state)
            return 1;
    }
}

static ULONG mempool_pointer_bucket(void *pointer)
{
    ULONG_PTR value = (ULONG_PTR)pointer / (ULONG_PTR)MEMPOOL_PAGE_SIZE;

    value ^= value >> 7;
#if defined(_WIN64) || defined(_M_AMD64) || defined(_M_ARM64) || \
    defined(__x86_64__) || defined(__aarch64__)
    value ^= value >> 32;
#endif
    return (ULONG)value & MEMPOOL_REGISTRY_BUCKET_MASK;
}

/* Side descriptors are not page-aligned, so hash their actual metadata
 * address rather than the backing base used by Mempool_Free. */
static ULONG mempool_identity_bucket(MEMPOOL_BACKING *backing)
{
    ULONG_PTR value = (ULONG_PTR)backing / (ULONG_PTR)sizeof(void *);

    value ^= value >> 7;
#if defined(_WIN64) || defined(_M_AMD64) || defined(_M_ARM64) || \
    defined(__x86_64__) || defined(__aarch64__)
    value ^= value >> 32;
#endif
    return (ULONG)value & MEMPOOL_REGISTRY_BUCKET_MASK;
}

/* Hot paths validate the global and target-bucket count/check pairs in O(1),
 * then walk exactly that bucket's trusted count.  PASSIVE create/destroy paths
 * additionally reconcile the complete fixed-size table with its global total.
 * No pointer walk ever uses an unchecked mutable count. */
static int mempool_count_pair_valid(ULONG count, ULONG check, ULONG limit)
{
    return count <= limit && count + check == 0;
}

static int mempool_count_entry_valid_locked(
    const ULONG *counts,
    const ULONG *checks,
    ULONG bucket,
    ULONG total,
    ULONG total_check)
{
    if (counts == (const ULONG *)0 || checks == (const ULONG *)0 ||
        bucket >= MEMPOOL_REGISTRY_BUCKET_COUNT ||
        !mempool_count_pair_valid(total, total_check,
                                  MEMPOOL_REGISTRY_CAPACITY) ||
        !mempool_count_pair_valid(counts[bucket], checks[bucket],
                                  MEMPOOL_REGISTRY_BUCKET_LIMIT) ||
        counts[bucket] > total)
        return 0;
    return 1;
}

static int mempool_count_table_valid_locked(
    const ULONG *counts,
    const ULONG *checks,
    ULONG total,
    ULONG total_check)
{
    ULONG sum = 0;
    ULONG bucket = 0;
    ULONG count = 0;

    if (counts == (const ULONG *)0 || checks == (const ULONG *)0 ||
        !mempool_count_pair_valid(total, total_check,
                                  MEMPOOL_REGISTRY_CAPACITY))
        return 0;
    for (bucket = 0; bucket < MEMPOOL_REGISTRY_BUCKET_COUNT; ++bucket) {
        count = counts[bucket];
        if (!mempool_count_pair_valid(count, checks[bucket],
                                      MEMPOOL_REGISTRY_BUCKET_LIMIT) ||
            sum > total ||
            count > total - sum)
            return 0;
        sum += count;
    }
    return sum == total;
}

static int mempool_registration_count_valid_locked(ULONG bucket)
{
    return mempool_count_entry_valid_locked(
               mempool_registration_bucket_counts,
               mempool_registration_bucket_checks, bucket,
               mempool_registration_count,
               mempool_registration_count_check);
}

static int mempool_registration_counts_full_valid_locked(void)
{
    return mempool_count_table_valid_locked(
               mempool_registration_bucket_counts,
               mempool_registration_bucket_checks,
               mempool_registration_count,
               mempool_registration_count_check);
}

static int mempool_backing_base_count_valid_locked(ULONG bucket)
{
    return mempool_count_entry_valid_locked(
               mempool_backing_base_bucket_counts,
               mempool_backing_base_bucket_checks, bucket,
               mempool_backing_registry_count,
               mempool_backing_registry_count_check);
}

static int mempool_backing_identity_count_valid_locked(ULONG bucket)
{
    return mempool_count_entry_valid_locked(
               mempool_backing_identity_bucket_counts,
               mempool_backing_identity_bucket_checks, bucket,
               mempool_backing_registry_count,
               mempool_backing_registry_count_check);
}

static int mempool_backing_counts_full_valid_locked(void)
{
    return mempool_count_table_valid_locked(
               mempool_backing_base_bucket_counts,
               mempool_backing_base_bucket_checks,
               mempool_backing_registry_count,
               mempool_backing_registry_count_check) &&
           mempool_count_table_valid_locked(
               mempool_backing_identity_bucket_counts,
               mempool_backing_identity_bucket_checks,
               mempool_backing_registry_count,
               mempool_backing_registry_count_check);
}

/* Return nonzero only when the complete target bucket is structurally valid.
 * A NULL result with a true return is an ordinary miss; a false return is
 * corruption.  Scanning the whole bounded bucket also rejects duplicates and
 * cycles before a caller changes registry state. */
static int mempool_registry_lookup_locked(
    POOL *pool,
    MEMPOOL_REGISTRATION **result,
    MEMPOOL_REGISTRATION **result_previous,
    ULONG *entry_count)
{
    MEMPOOL_REGISTRATION *current = (MEMPOOL_REGISTRATION *)0;
    MEMPOOL_REGISTRATION *found = (MEMPOOL_REGISTRATION *)0;
    MEMPOOL_REGISTRATION *found_previous = (MEMPOOL_REGISTRATION *)0;
    MEMPOOL_REGISTRATION *previous = (MEMPOOL_REGISTRATION *)0;
    ULONG bucket = 0;
    ULONG expected = 0;
    ULONG matches = 0;
    ULONG visited = 0;

    if (result != (MEMPOOL_REGISTRATION **)0)
        *result = (MEMPOOL_REGISTRATION *)0;
    if (result_previous != (MEMPOOL_REGISTRATION **)0)
        *result_previous = (MEMPOOL_REGISTRATION *)0;
    if (entry_count != (ULONG *)0)
        *entry_count = 0;
    if (pool == (POOL *)0)
        return 0;
    bucket = mempool_pointer_bucket(pool);
    if (!mempool_registration_count_valid_locked(bucket))
        return 0;
    expected = mempool_registration_bucket_counts[bucket];
    current = mempool_registry[bucket];
    while (visited < expected) {
        if (current == (MEMPOOL_REGISTRATION *)0 ||
            current->pool == (POOL *)0 || current->next == current ||
            mempool_pointer_bucket(current->pool) != bucket ||
            (current->pool_type != (ULONG)MEMPOOL_PAGED &&
             current->pool_type != (ULONG)MEMPOOL_NONPAGED))
            return 0;
        if (current->pool == pool) {
            ++matches;
            found = current;
            found_previous = previous;
        }
        previous = current;
        current = current->next;
        ++visited;
    }
    if (current != (MEMPOOL_REGISTRATION *)0 || matches > 1)
        return 0;
    if (result != (MEMPOOL_REGISTRATION **)0)
        *result = found;
    if (result_previous != (MEMPOOL_REGISTRATION **)0)
        *result_previous = found_previous;
    if (entry_count != (ULONG *)0)
        *entry_count = expected;
    return 1;
}

/* The caller obtained registration/previous from a complete lookup while
 * holding the same lock.  Recheck only the O(1) mutation boundary, then close
 * admission and unlink without another bucket traversal.  A negative result
 * denotes structural corruption; zero is an already-closing lifecycle. */
static int mempool_registry_close_remove_locked(
    POOL *pool,
    MEMPOOL_REGISTRATION *registration,
    MEMPOOL_REGISTRATION *previous)
{
    ULONG bucket = 0;

    if (pool == (POOL *)0 || registration == (MEMPOOL_REGISTRATION *)0 ||
        registration->pool != pool)
        return -1;
    bucket = mempool_pointer_bucket(pool);
    if (!mempool_registration_count_valid_locked(bucket) ||
        mempool_registration_count == 0 ||
        mempool_registration_bucket_counts[bucket] == 0 ||
        registration->next == registration ||
        (previous == (MEMPOOL_REGISTRATION *)0 &&
         mempool_registry[bucket] != registration) ||
        (previous != (MEMPOOL_REGISTRATION *)0 &&
         (previous == registration || previous->next != registration ||
          previous->pool == (POOL *)0 ||
          mempool_pointer_bucket(previous->pool) != bucket)))
        return -1;
    if (!mempool_begin_destroy(registration))
        return 0;
    if (previous == (MEMPOOL_REGISTRATION *)0)
        mempool_registry[bucket] = registration->next;
    else
        previous->next = registration->next;
    registration->next = (MEMPOOL_REGISTRATION *)0;
    --mempool_registration_bucket_counts[bucket];
    ++mempool_registration_bucket_checks[bucket];
    --mempool_registration_count;
    ++mempool_registration_count_check;
    return 1;
}

static void *mempool_registry_alloc(void)
{
#ifdef _KERNEL_MODE
    return Allocator_Malloc((BOOLEAN)1,
                            sizeof(MEMPOOL_REGISTRATION),
                            MEMPOOL_POOL_TAG);
#else
    return Allocator_Malloc(sizeof(MEMPOOL_REGISTRATION));
#endif
}

static void mempool_registry_free(MEMPOOL_REGISTRATION *registration)
{
    if (registration == (MEMPOOL_REGISTRATION *)0)
        return;
#ifdef _KERNEL_MODE
    Allocator_Free(registration, MEMPOOL_POOL_TAG);
#else
    Allocator_Free(registration);
#endif
}

static MEMPOOL_BACKING *mempool_backing_identity_find_locked(
    MEMPOOL_BACKING *identity);

static MEMPOOL_BACKING *mempool_backing_find_locked(void *base)
{
    MEMPOOL_BACKING *backing = (MEMPOOL_BACKING *)0;
    MEMPOOL_BACKING *found = (MEMPOOL_BACKING *)0;
    ULONG bucket = mempool_pointer_bucket(base);
    ULONG expected = 0;
    ULONG matches = 0;
    ULONG visited = 0;

    if (!mempool_backing_base_count_valid_locked(bucket))
        return (MEMPOOL_BACKING *)0;
    expected = mempool_backing_base_bucket_counts[bucket];
    backing = mempool_backing_registry[bucket];
    while (visited < expected) {
        if (backing == (MEMPOOL_BACKING *)0 ||
            backing->base == (void *)0 || backing->hash_next == backing ||
            mempool_pointer_bucket(backing->base) != bucket ||
            backing->registration == (MEMPOOL_REGISTRATION *)0 ||
            InterlockedCompareExchange(&backing->closing, 0, 0) != 0 ||
            InterlockedCompareExchange(&backing->references, 0, 0) <= 0)
            return (MEMPOOL_BACKING *)0;
        if (backing->base == base) {
            ++matches;
            found = backing;
        }
        backing = backing->hash_next;
        ++visited;
    }
    if (backing != (MEMPOOL_BACKING *)0 || matches > 1)
        return (MEMPOOL_BACKING *)0;
    if (found != (MEMPOOL_BACKING *)0 &&
        mempool_backing_identity_find_locked(found) != found)
        return (MEMPOOL_BACKING *)0;
    return found;
}

static MEMPOOL_BACKING *mempool_backing_identity_find_locked(
    MEMPOOL_BACKING *identity)
{
    MEMPOOL_BACKING *backing = (MEMPOOL_BACKING *)0;
    MEMPOOL_BACKING *found = (MEMPOOL_BACKING *)0;
    ULONG bucket = mempool_identity_bucket(identity);
    ULONG expected = 0;
    ULONG matches = 0;
    ULONG visited = 0;

    if (!mempool_backing_identity_count_valid_locked(bucket))
        return (MEMPOOL_BACKING *)0;
    expected = mempool_backing_identity_bucket_counts[bucket];
    backing = mempool_backing_identity_registry[bucket];
    while (visited < expected) {
        if (backing == (MEMPOOL_BACKING *)0 ||
            backing->identity_next == backing ||
            mempool_identity_bucket(backing) != bucket)
            return (MEMPOOL_BACKING *)0;
        if (backing == identity) {
            ++matches;
            found = backing;
        }
        backing = backing->identity_next;
        ++visited;
    }
    if (backing != (MEMPOOL_BACKING *)0 || matches > 1)
        return (MEMPOOL_BACKING *)0;
    return found;
}

/* Insertion needs a tri-state result: a complete, valid bucket with no match
 * is different from a corrupt bucket whose lookup merely failed.  These two
 * helpers perform the complete bounded checks before either index is changed. */
static int mempool_backing_base_can_insert_locked(void *base)
{
    MEMPOOL_BACKING *current = (MEMPOOL_BACKING *)0;
    ULONG bucket = mempool_pointer_bucket(base);
    ULONG expected = 0;
    ULONG visited = 0;

    if (!mempool_backing_base_count_valid_locked(bucket))
        return 0;
    expected = mempool_backing_base_bucket_counts[bucket];
    if (expected >= MEMPOOL_REGISTRY_BUCKET_LIMIT)
        return 0;
    current = mempool_backing_registry[bucket];
    while (visited < expected) {
        if (current == (MEMPOOL_BACKING *)0 ||
            current->base == (void *)0 || current->hash_next == current ||
            mempool_pointer_bucket(current->base) != bucket ||
            current->registration == (MEMPOOL_REGISTRATION *)0 ||
            InterlockedCompareExchange(&current->closing, 0, 0) != 0 ||
            InterlockedCompareExchange(&current->references, 0, 0) <= 0 ||
            current->base == base)
            return 0;
        current = current->hash_next;
        ++visited;
    }
    return current == (MEMPOOL_BACKING *)0;
}

static int mempool_backing_identity_can_insert_locked(
    MEMPOOL_BACKING *identity)
{
    MEMPOOL_BACKING *current = (MEMPOOL_BACKING *)0;
    ULONG bucket = mempool_identity_bucket(identity);
    ULONG expected = 0;
    ULONG visited = 0;

    if (!mempool_backing_identity_count_valid_locked(bucket))
        return 0;
    expected = mempool_backing_identity_bucket_counts[bucket];
    if (expected >= MEMPOOL_REGISTRY_BUCKET_LIMIT)
        return 0;
    current = mempool_backing_identity_registry[bucket];
    while (visited < expected) {
        if (current == (MEMPOOL_BACKING *)0 || current == identity ||
            current->identity_next == current ||
            mempool_identity_bucket(current) != bucket ||
            current->base == (void *)0 ||
            current->registration == (MEMPOOL_REGISTRATION *)0 ||
            InterlockedCompareExchange(&current->closing, 0, 0) != 0 ||
            InterlockedCompareExchange(&current->references, 0, 0) <= 0)
            return 0;
        current = current->identity_next;
        ++visited;
    }
    return current == (MEMPOOL_BACKING *)0;
}

/* Validate the complete target bucket before unlinking.  This rejects a
 * duplicate target or a cycle such as target->other->target, either of which
 * would otherwise leave a published pointer to storage about to be freed. */
static int mempool_backing_base_bucket_validate_locked(
    ULONG bucket,
    MEMPOOL_BACKING *target,
    MEMPOOL_BACKING **target_previous)
{
    MEMPOOL_BACKING *current = (MEMPOOL_BACKING *)0;
    MEMPOOL_BACKING *previous = (MEMPOOL_BACKING *)0;
    ULONG expected = 0;
    ULONG found = 0;
    ULONG visited = 0;

    if (target_previous != (MEMPOOL_BACKING **)0)
        *target_previous = (MEMPOOL_BACKING *)0;
    if (!mempool_backing_base_count_valid_locked(bucket))
        return 0;
    expected = mempool_backing_base_bucket_counts[bucket];
    current = mempool_backing_registry[bucket];
    while (visited < expected) {
        if (current == (MEMPOOL_BACKING *)0 ||
            current->base == (void *)0 || current->hash_next == current ||
            mempool_pointer_bucket(current->base) != bucket ||
            current->registration == (MEMPOOL_REGISTRATION *)0 ||
            InterlockedCompareExchange(&current->closing, 0, 0) != 0 ||
            InterlockedCompareExchange(&current->references, 0, 0) <= 0)
            return 0;
        if (current == target) {
            ++found;
            if (target_previous != (MEMPOOL_BACKING **)0)
                *target_previous = previous;
        }
        previous = current;
        current = current->hash_next;
        ++visited;
    }
    return current == (MEMPOOL_BACKING *)0 && found == 1;
}

static int mempool_backing_identity_bucket_validate_locked(
    ULONG bucket,
    MEMPOOL_BACKING *target,
    MEMPOOL_BACKING **target_previous)
{
    MEMPOOL_BACKING *current = (MEMPOOL_BACKING *)0;
    MEMPOOL_BACKING *previous = (MEMPOOL_BACKING *)0;
    ULONG expected = 0;
    ULONG found = 0;
    ULONG visited = 0;

    if (target_previous != (MEMPOOL_BACKING **)0)
        *target_previous = (MEMPOOL_BACKING *)0;
    if (!mempool_backing_identity_count_valid_locked(bucket))
        return 0;
    expected = mempool_backing_identity_bucket_counts[bucket];
    current = mempool_backing_identity_registry[bucket];
    while (visited < expected) {
        if (current == (MEMPOOL_BACKING *)0 ||
            current->identity_next == current ||
            mempool_identity_bucket(current) != bucket ||
            current->base == (void *)0 ||
            current->registration == (MEMPOOL_REGISTRATION *)0 ||
            InterlockedCompareExchange(&current->closing, 0, 0) != 0 ||
            InterlockedCompareExchange(&current->references, 0, 0) <= 0)
            return 0;
        if (current == target) {
            ++found;
            if (target_previous != (MEMPOOL_BACKING **)0)
                *target_previous = previous;
        }
        previous = current;
        current = current->identity_next;
        ++visited;
    }
    return current == (MEMPOOL_BACKING *)0 && found == 1;
}

/*
 * Destroy calls this only after admission has closed and active operations
 * have drained.  The owner count is accepted only after the two global backing
 * count tables reconcile and remains capped by the compile-time capacity, so a
 * damaged count never becomes an unbounded walk.  Normal DISPATCH_LEVEL
 * retirement deliberately uses local owner links instead of this full pass.
 */
static int mempool_backing_owner_validate(
    MEMPOOL_REGISTRATION *registration,
    MEMPOOL_BACKING *target,
    MEMPOOL_BACKING **target_previous)
{
    MEMPOOL_BACKING *current = (MEMPOOL_BACKING *)0;
    MEMPOOL_BACKING *next = (MEMPOOL_BACKING *)0;
    MEMPOOL_BACKING *previous = (MEMPOOL_BACKING *)0;
    MEMPOOL_BACKING *found_previous = (MEMPOOL_BACKING *)0;
    ULONG count = 0;
    ULONG index = 0;
    ULONG published_count = 0;
    ULONG found = 0;
    int counts_valid = 0;
    int node_valid = 0;
    MEMPOOL_REGISTRY_LOCK_DECL

    if (target_previous != (MEMPOOL_BACKING **)0)
        *target_previous = (MEMPOOL_BACKING *)0;
    if (registration == (MEMPOOL_REGISTRATION *)0)
        return 0;

    /* backing_count is side metadata too, so it cannot be trusted as the sole
       cycle bound.  Snapshot it with the exact global publication count and
       reject an impossible per-pool count before following the first owner
       link.  This keeps a forged ULONG_MAX count plus an owner cycle from
       turning PASSIVE_LEVEL teardown into a practically unbounded loop. */
    MEMPOOL_REGISTRY_LOCK();
    counts_valid = mempool_backing_counts_full_valid_locked();
    if (counts_valid != 0) {
        count = registration->backing_count;
        published_count = mempool_backing_registry_count;
        current = registration->backings;
    }
    MEMPOOL_REGISTRY_UNLOCK();
    if (counts_valid == 0 || count == 0 ||
        count > MEMPOOL_REGISTRY_CAPACITY ||
        published_count == 0 ||
        published_count > MEMPOOL_REGISTRY_CAPACITY ||
        count > published_count ||
        current == (MEMPOOL_BACKING *)0)
        return 0;

    while (index < count) {
        /* current may be a stale or cross-registration link.  Validate and
           snapshot it while the identity index pins publication; only nodes
           from this already-closed registration are used after unlocking. */
        MEMPOOL_REGISTRY_LOCK();
        node_valid = current != (MEMPOOL_BACKING *)0 &&
                     mempool_backing_identity_find_locked(current) == current;
        if (node_valid != 0) {
            node_valid = current->owner_prev == previous &&
                         current->registration == registration &&
                         InterlockedCompareExchange(&current->closing,
                                                    0, 0) == 0 &&
                         InterlockedCompareExchange(&current->references,
                                                    0, 0) > 0;
            if (node_valid != 0)
                next = current->owner_next;
        }
        MEMPOOL_REGISTRY_UNLOCK();
        if (node_valid == 0)
            return 0;
        if (current == target) {
            ++found;
            found_previous = previous;
        }
        previous = current;
        current = next;
        ++index;
    }
    if (current != (MEMPOOL_BACKING *)0 || found != 1)
        return 0;
    if (target_previous != (MEMPOOL_BACKING **)0)
        *target_previous = found_previous;
    return 1;
}

static void mempool_backing_prepare(MEMPOOL_BACKING *backing,
                                    MEMPOOL_REGISTRATION *registration,
                                    void *base, void *object, ULONG size,
                                    ULONG tag, ULONG kind)
{
    backing->hash_next = (MEMPOOL_BACKING *)0;
    backing->identity_next = (MEMPOOL_BACKING *)0;
    backing->owner_next = (MEMPOOL_BACKING *)0;
    backing->owner_prev = (MEMPOOL_BACKING *)0;
    backing->base = base;
    backing->object = object;
    backing->registration = registration;
    backing->size = size;
    backing->tag = tag;
    backing->kind = kind;
    backing->references = 0;
    backing->closing = 0;
}

static int mempool_backing_insert_locked(MEMPOOL_BACKING *backing)
{
    ULONG bucket = 0;
    ULONG identity_bucket = 0;
    MEMPOOL_REGISTRATION *registration = (MEMPOOL_REGISTRATION *)0;
    MEMPOOL_BACKING *owner_head = (MEMPOOL_BACKING *)0;

    if (backing == (MEMPOOL_BACKING *)0 ||
        backing->registration == (MEMPOOL_REGISTRATION *)0)
        return 0;
    registration = backing->registration;
    /* Check every failure condition before touching either index. */
    owner_head = registration->backings;
    if (registration->backing_count >= MEMPOOL_REGISTRY_CAPACITY ||
        mempool_backing_registry_count >= MEMPOOL_REGISTRY_CAPACITY ||
        ((registration->backing_count == 0) !=
         (owner_head == (MEMPOOL_BACKING *)0)) ||
        backing->hash_next != (MEMPOOL_BACKING *)0 ||
        backing->identity_next != (MEMPOOL_BACKING *)0 ||
        backing->owner_next != (MEMPOOL_BACKING *)0 ||
        backing->owner_prev != (MEMPOOL_BACKING *)0 ||
        InterlockedCompareExchange(&backing->references, 0, 0) != 0 ||
        InterlockedCompareExchange(&backing->closing, 0, 0) != 0)
        return 0;

    bucket = mempool_pointer_bucket(backing->base);
    identity_bucket = mempool_identity_bucket(backing);
    if (!mempool_backing_base_can_insert_locked(backing->base) ||
        !mempool_backing_identity_can_insert_locked(backing) ||
        (owner_head != (MEMPOOL_BACKING *)0 &&
         (mempool_backing_identity_find_locked(owner_head) != owner_head ||
          owner_head->registration != registration ||
          owner_head->owner_prev != (MEMPOOL_BACKING *)0 ||
          owner_head->closing != 0 ||
          InterlockedCompareExchange(&owner_head->references, 0, 0) <= 0)))
        return 0;
    backing->references = 1; /* Registry ownership. */
    backing->hash_next = mempool_backing_registry[bucket];
    mempool_backing_registry[bucket] = backing;
    backing->identity_next =
        mempool_backing_identity_registry[identity_bucket];
    mempool_backing_identity_registry[identity_bucket] = backing;
    backing->owner_next = owner_head;
    backing->owner_prev = (MEMPOOL_BACKING *)0;
    if (owner_head != (MEMPOOL_BACKING *)0)
        owner_head->owner_prev = backing;
    registration->backings = backing;
    ++registration->backing_count;
    ++mempool_backing_base_bucket_counts[bucket];
    --mempool_backing_base_bucket_checks[bucket];
    ++mempool_backing_identity_bucket_counts[identity_bucket];
    --mempool_backing_identity_bucket_checks[identity_bucket];
    ++mempool_backing_registry_count;
    --mempool_backing_registry_count_check;
    return 1;
}

static int mempool_backing_publish(MEMPOOL_BACKING *backing)
{
    MEMPOOL_REGISTRATION *registration = backing->registration;
    LONG state = 0;
    int published = 0;
    MEMPOOL_REGISTRY_LOCK_DECL

    MEMPOOL_REGISTRY_LOCK();
    state = InterlockedCompareExchange(&registration->lifecycle_state, 0, 0);
    if ((state & MEMPOOL_LIFECYCLE_CLOSING) == 0)
        published = mempool_backing_insert_locked(backing);
    MEMPOOL_REGISTRY_UNLOCK();
    return published;
}

static LONG mempool_backing_retire_internal(
    MEMPOOL_REGISTRATION *registration,
    MEMPOOL_BACKING *backing)
{
    MEMPOOL_BACKING *owner_previous = (MEMPOOL_BACKING *)0;
    MEMPOOL_BACKING *owner_next = (MEMPOOL_BACKING *)0;
    MEMPOOL_BACKING *previous = (MEMPOOL_BACKING *)0;
    MEMPOOL_BACKING *identity_previous = (MEMPOOL_BACKING *)0;
    ULONG bucket = 0;
    ULONG identity_bucket = 0;
    int base_bucket_valid = 0;
    int identity_bucket_valid = 0;
    LONG remaining = -1;
    MEMPOOL_REGISTRY_LOCK_DECL

    if (registration == (MEMPOOL_REGISTRATION *)0 ||
        backing == (MEMPOOL_BACKING *)0)
        return -1;

    MEMPOOL_REGISTRY_LOCK();
    if (backing->closing == 0 && backing->registration == registration &&
        registration->backing_count != 0 &&
        registration->backing_count <= MEMPOOL_REGISTRY_CAPACITY &&
        mempool_backing_registry_count != 0 &&
        mempool_backing_registry_count <= MEMPOOL_REGISTRY_CAPACITY &&
        mempool_backing_identity_find_locked(backing) == backing &&
        InterlockedCompareExchange(&backing->references, 0, 0) > 0) {
        owner_previous = backing->owner_prev;
        owner_next = backing->owner_next;
        bucket = mempool_pointer_bucket(backing->base);
        identity_bucket = mempool_identity_bucket(backing);
        base_bucket_valid = mempool_backing_base_bucket_validate_locked(
                                bucket, backing, &previous);
        identity_bucket_valid =
            mempool_backing_identity_bucket_validate_locked(
                identity_bucket, backing, &identity_previous);
        if (base_bucket_valid != 0 && identity_bucket_valid != 0) {
            /* Validate both indexes before changing either one.  A failed
               retire must leave the storage published and owned; leaking is
               safer than unlinking only half of a corrupted registration. */
            if (owner_previous == backing || owner_next == backing ||
                (owner_previous != (MEMPOOL_BACKING *)0 &&
                 owner_previous == owner_next) ||
                ((registration->backings == backing) !=
                 (owner_previous == (MEMPOOL_BACKING *)0)) ||
                (owner_next != (MEMPOOL_BACKING *)0 &&
                 registration->backings == owner_next) ||
                (owner_previous != (MEMPOOL_BACKING *)0 &&
                 (mempool_backing_identity_find_locked(owner_previous) !=
                      owner_previous ||
                  owner_previous->registration != registration ||
                  owner_previous->closing != 0 ||
                  InterlockedCompareExchange(&owner_previous->references,
                                             0, 0) <= 0 ||
                  owner_previous->owner_next != backing)) ||
                (owner_next != (MEMPOOL_BACKING *)0 &&
                 (mempool_backing_identity_find_locked(owner_next) !=
                      owner_next ||
                  owner_next->registration != registration ||
                  owner_next->closing != 0 ||
                  InterlockedCompareExchange(&owner_next->references,
                                             0, 0) <= 0 ||
                  owner_next->owner_prev != backing)) ||
                registration->backing_count == 0 ||
                mempool_backing_registry_count == 0) {
                MEMPOOL_REGISTRY_UNLOCK();
                return -1;
            }
            if (previous == (MEMPOOL_BACKING *)0)
                mempool_backing_registry[bucket] = backing->hash_next;
            else
                previous->hash_next = backing->hash_next;
            if (identity_previous == (MEMPOOL_BACKING *)0)
                mempool_backing_identity_registry[identity_bucket] =
                    backing->identity_next;
            else
                identity_previous->identity_next = backing->identity_next;
            if (owner_previous != (MEMPOOL_BACKING *)0)
                owner_previous->owner_next = owner_next;
            else
                registration->backings = owner_next;
            if (owner_next != (MEMPOOL_BACKING *)0)
                owner_next->owner_prev = owner_previous;
            backing->hash_next = (MEMPOOL_BACKING *)0;
            backing->identity_next = (MEMPOOL_BACKING *)0;
            backing->owner_next = (MEMPOOL_BACKING *)0;
            backing->owner_prev = (MEMPOOL_BACKING *)0;
            --registration->backing_count;
            --mempool_backing_base_bucket_counts[bucket];
            ++mempool_backing_base_bucket_checks[bucket];
            --mempool_backing_identity_bucket_counts[identity_bucket];
            ++mempool_backing_identity_bucket_checks[identity_bucket];
            --mempool_backing_registry_count;
            ++mempool_backing_registry_count_check;
            (void)InterlockedExchange(&backing->closing, 1);
            remaining = InterlockedDecrement(&backing->references);
        }
    }
    MEMPOOL_REGISTRY_UNLOCK();
    return remaining;
}

/* Normal callers hold the pool lock.  Retirement validates only the target's
 * local owner links plus its two hash buckets; it never scans all pool owners
 * at DISPATCH_LEVEL. */
static LONG mempool_backing_retire(MEMPOOL_REGISTRATION *registration,
                                   MEMPOOL_BACKING *backing)
{
    return mempool_backing_retire_internal(registration, backing);
}

/* Destroy has already performed a complete immutable-chain preflight. */
static LONG mempool_backing_retire_prevalidated(
    MEMPOOL_REGISTRATION *registration,
    MEMPOOL_BACKING *backing)
{
    return mempool_backing_retire_internal(registration, backing);
}

/* ------------------------------------------------------------------------- */
/* Bounded small-page indexes                                                */
/* ------------------------------------------------------------------------- */

/* The pool lock is held by every caller below.  Identity checks additionally
 * take the registry lock, preserving the global pool -> registry lock order.
 * No untrusted page-list/bin pointer is dereferenced until its embedded
 * backing address has been found in the independent identity registry. */
static int mempool_page_node_valid_locked(POOL *pool, MEMPOOL_PAGE *page)
{
    MEMPOOL_BACKING *backing = (MEMPOOL_BACKING *)0;
    ULONG_PTR page_value = (ULONG_PTR)0;

    if (pool == (POOL *)0 || page == (MEMPOOL_PAGE *)0)
        return 0;
    page_value = (ULONG_PTR)page;
    if (page_value > ~(ULONG_PTR)0 -
                     (ULONG_PTR)offsetof(MEMPOOL_PAGE, backing))
        return 0;
    backing = (MEMPOOL_BACKING *)(page_value +
              (ULONG_PTR)offsetof(MEMPOOL_PAGE, backing));
    /* Base lookup validates its complete bounded bucket and cross-checks the
       matching node in the identity index before returning it. */
    if (mempool_backing_find_locked(page) != backing)
        return 0;
    return backing->base == page && backing->object == page &&
           backing->registration == pool->registration &&
           backing->size == MEMPOOL_PAGE_SIZE &&
           backing->tag == pool->eyecatcher &&
           backing->kind == MEMPOOL_BACKING_PAGE &&
           InterlockedCompareExchange(&backing->closing, 0, 0) == 0 &&
           InterlockedCompareExchange(&backing->references, 0, 0) > 0 &&
           page->pool == pool && page->eyecatcher == pool->eyecatcher;
}

static int mempool_run_bin_count_valid(POOL *pool, ULONG run)
{
    USHORT count = 0;
    USHORT check = 0;

    if (pool == (POOL *)0 || run >= MEMPOOL_RUN_BIN_COUNT)
        return 0;
    count = pool->run_bin_counts[run];
    check = pool->run_bin_checks[run];
    return (USHORT)(count + check) == (USHORT)0 &&
           ((count == (USHORT)0) ==
            (pool->run_bins[run] == (MEMPOOL_PAGE *)0));
}

/* Validate all compact counters in a fixed number of iterations.  This is
 * independent of the number of pages and therefore safe under the pool spin
 * lock at DISPATCH_LEVEL.  The ownership chain itself is deliberately not
 * traversed here. */
static int mempool_page_index_counts_valid(POOL *pool)
{
    ULONG run = 0;
    ULONG sum = 0;
    ULONG count = 0;

    if (pool == (POOL *)0 ||
        pool->page_count + pool->page_count_check != 0 ||
        pool->page_count == 0 ||
        pool->page_count > MEMPOOL_PAGE_COUNT_LIMIT ||
        pool->pages.count < 0 ||
        (ULONG)pool->pages.count != pool->page_count ||
        pool->pages.head == (LIST_ELEM *)0 ||
        pool->pages.tail == (LIST_ELEM *)0 ||
        pool->full_pages.head != (LIST_ELEM *)0 ||
        pool->full_pages.tail != (LIST_ELEM *)0 ||
        pool->full_pages.count != 0)
        return 0;
    while (run < MEMPOOL_RUN_BIN_COUNT) {
        if (!mempool_run_bin_count_valid(pool, run))
            return 0;
        count = (ULONG)pool->run_bin_counts[run];
        if (count > pool->page_count || sum > pool->page_count - count)
            return 0;
        sum += count;
        ++run;
    }
    return sum == pool->page_count;
}

/* Validate a page and the two links that a bin removal would overwrite. */
static int mempool_run_bin_page_validate(POOL *pool, MEMPOOL_PAGE *page,
                                         ULONG run)
{
    MEMPOOL_PAGE *previous = (MEMPOOL_PAGE *)0;
    MEMPOOL_PAGE *next = (MEMPOOL_PAGE *)0;
    USHORT count = 0;
    int valid = 1;
    MEMPOOL_REGISTRY_LOCK_DECL

    if (!mempool_run_bin_count_valid(pool, run) ||
        page == (MEMPOOL_PAGE *)0)
        return 0;
    count = pool->run_bin_counts[run];
    if (count == (USHORT)0)
        return 0;

    MEMPOOL_REGISTRY_LOCK();
    if (!mempool_page_node_valid_locked(pool, page)) {
        valid = 0;
    } else {
        previous = page->bin_prev;
        next = page->bin_next;
        if (page->max_free_run != (USHORT)run || previous == page ||
            next == page ||
            (previous != (MEMPOOL_PAGE *)0 && previous == next) ||
            ((pool->run_bins[run] == page) !=
             (previous == (MEMPOOL_PAGE *)0)) ||
            ((count == (USHORT)1) !=
             (previous == (MEMPOOL_PAGE *)0 &&
              next == (MEMPOOL_PAGE *)0))) {
            valid = 0;
        } else if (previous != (MEMPOOL_PAGE *)0 &&
                   (!mempool_page_node_valid_locked(pool, previous) ||
                    previous->max_free_run != (USHORT)run ||
                    previous->bin_next != page)) {
            valid = 0;
        } else if (next != (MEMPOOL_PAGE *)0 &&
                   (!mempool_page_node_valid_locked(pool, next) ||
                    next->max_free_run != (USHORT)run ||
                    next->bin_prev != page)) {
            valid = 0;
        }
    }
    MEMPOOL_REGISTRY_UNLOCK();
    return valid;
}

static int mempool_run_bin_insert_validate(POOL *pool, ULONG run)
{
    USHORT count = 0;

    if (!mempool_run_bin_count_valid(pool, run))
        return 0;
    count = pool->run_bin_counts[run];
    if ((ULONG)count >= MEMPOOL_PAGE_COUNT_LIMIT)
        return 0;
    if (count == (USHORT)0)
        return 1;
    return mempool_run_bin_page_validate(pool, pool->run_bins[run], run);
}

static void mempool_run_bin_remove_unchecked(POOL *pool,
                                              MEMPOOL_PAGE *page,
                                              ULONG run)
{
    MEMPOOL_PAGE *previous = page->bin_prev;
    MEMPOOL_PAGE *next = page->bin_next;

    if (previous != (MEMPOOL_PAGE *)0)
        previous->bin_next = next;
    else
        pool->run_bins[run] = next;
    if (next != (MEMPOOL_PAGE *)0)
        next->bin_prev = previous;
    page->bin_next = (MEMPOOL_PAGE *)0;
    page->bin_prev = (MEMPOOL_PAGE *)0;
    --pool->run_bin_counts[run];
    ++pool->run_bin_checks[run];
}

static void mempool_run_bin_insert_unchecked(POOL *pool,
                                              MEMPOOL_PAGE *page,
                                              ULONG run)
{
    MEMPOOL_PAGE *head = pool->run_bins[run];

    page->bin_prev = (MEMPOOL_PAGE *)0;
    page->bin_next = head;
    if (head != (MEMPOOL_PAGE *)0)
        head->bin_prev = page;
    pool->run_bins[run] = page;
    ++pool->run_bin_counts[run];
    --pool->run_bin_checks[run];
}

/* Validate the ownership-list boundary before publishing a newly allocated
 * page.  The caller performs this while the new page is still private. */
static int mempool_page_list_insert_validate(POOL *pool)
{
    MEMPOOL_PAGE *head = (MEMPOOL_PAGE *)0;
    MEMPOOL_PAGE *tail = (MEMPOOL_PAGE *)0;
    MEMPOOL_PAGE *head_next = (MEMPOOL_PAGE *)0;
    MEMPOOL_PAGE *tail_previous = (MEMPOOL_PAGE *)0;
    int valid = 1;
    MEMPOOL_REGISTRY_LOCK_DECL

    if (!mempool_page_index_counts_valid(pool) ||
        pool->page_count >= MEMPOOL_PAGE_COUNT_LIMIT)
        return 0;
    head = (MEMPOOL_PAGE *)pool->pages.head;
    tail = (MEMPOOL_PAGE *)pool->pages.tail;

    MEMPOOL_REGISTRY_LOCK();
    if (!mempool_page_node_valid_locked(pool, head) ||
        !mempool_page_node_valid_locked(pool, tail) ||
        head->list_elem.prev != (LIST_ELEM *)0 ||
        tail->list_elem.next != (LIST_ELEM *)0 ||
        ((pool->page_count == 1) != (head == tail))) {
        valid = 0;
    } else if (pool->page_count == 1) {
        if (head->list_elem.next != (LIST_ELEM *)0 ||
            head->list_elem.prev != (LIST_ELEM *)0)
            valid = 0;
    } else {
        head_next = (MEMPOOL_PAGE *)head->list_elem.next;
        tail_previous = (MEMPOOL_PAGE *)tail->list_elem.prev;
        if (head_next == (MEMPOOL_PAGE *)0 ||
            tail_previous == (MEMPOOL_PAGE *)0 || head_next == head ||
            tail_previous == tail ||
            !mempool_page_node_valid_locked(pool, head_next) ||
            !mempool_page_node_valid_locked(pool, tail_previous) ||
            head_next->list_elem.prev != (LIST_ELEM *)head ||
            tail_previous->list_elem.next != (LIST_ELEM *)tail)
            valid = 0;
    }
    MEMPOOL_REGISTRY_UNLOCK();
    return valid;
}

/* Validate a page and every ownership-list pointer a removal will overwrite. */
static int mempool_page_list_remove_validate(POOL *pool, MEMPOOL_PAGE *page)
{
    MEMPOOL_PAGE *head = (MEMPOOL_PAGE *)0;
    MEMPOOL_PAGE *tail = (MEMPOOL_PAGE *)0;
    MEMPOOL_PAGE *previous = (MEMPOOL_PAGE *)0;
    MEMPOOL_PAGE *next = (MEMPOOL_PAGE *)0;
    int valid = 1;
    MEMPOOL_REGISTRY_LOCK_DECL

    if (!mempool_page_index_counts_valid(pool) ||
        page == (MEMPOOL_PAGE *)0)
        return 0;
    head = (MEMPOOL_PAGE *)pool->pages.head;
    tail = (MEMPOOL_PAGE *)pool->pages.tail;

    MEMPOOL_REGISTRY_LOCK();
    if (!mempool_page_node_valid_locked(pool, page)) {
        valid = 0;
    } else {
        previous = (MEMPOOL_PAGE *)page->list_elem.prev;
        next = (MEMPOOL_PAGE *)page->list_elem.next;
        if (previous == page || next == page ||
            (previous != (MEMPOOL_PAGE *)0 && previous == next) ||
            ((head == page) != (previous == (MEMPOOL_PAGE *)0)) ||
            ((tail == page) != (next == (MEMPOOL_PAGE *)0)) ||
            ((pool->page_count == 1) !=
             (previous == (MEMPOOL_PAGE *)0 &&
              next == (MEMPOOL_PAGE *)0)) ||
            !mempool_page_node_valid_locked(pool, head) ||
            !mempool_page_node_valid_locked(pool, tail) ||
            head->list_elem.prev != (LIST_ELEM *)0 ||
            tail->list_elem.next != (LIST_ELEM *)0 ||
            ((pool->page_count == 1) != (head == tail))) {
            valid = 0;
        } else if (previous != (MEMPOOL_PAGE *)0 &&
                   (!mempool_page_node_valid_locked(pool, previous) ||
                    previous->list_elem.next != (LIST_ELEM *)page)) {
            valid = 0;
        } else if (next != (MEMPOOL_PAGE *)0 &&
                   (!mempool_page_node_valid_locked(pool, next) ||
                    next->list_elem.prev != (LIST_ELEM *)page)) {
            valid = 0;
        }
    }
    MEMPOOL_REGISTRY_UNLOCK();
    return valid;
}

static void mempool_page_list_insert_unchecked(POOL *pool,
                                                MEMPOOL_PAGE *page)
{
    LIST_ELEM *head = pool->pages.head;

    page->list_elem.prev = (LIST_ELEM *)0;
    page->list_elem.next = head;
    if (head != (LIST_ELEM *)0)
        head->prev = (LIST_ELEM *)page;
    else
        pool->pages.tail = (LIST_ELEM *)page;
    pool->pages.head = (LIST_ELEM *)page;
    ++pool->pages.count;
    ++pool->page_count;
    --pool->page_count_check;
}

static void mempool_page_list_remove_unchecked(POOL *pool,
                                                MEMPOOL_PAGE *page)
{
    LIST_ELEM *previous = page->list_elem.prev;
    LIST_ELEM *next = page->list_elem.next;

    if (previous != (LIST_ELEM *)0)
        previous->next = next;
    else
        pool->pages.head = next;
    if (next != (LIST_ELEM *)0)
        next->prev = previous;
    else
        pool->pages.tail = previous;
    page->list_elem.next = (LIST_ELEM *)0;
    page->list_elem.prev = (LIST_ELEM *)0;
    --pool->pages.count;
    --pool->page_count;
    ++pool->page_count_check;
}

/* Derive the containing allocation from the pinned node itself.  The object
 * field is checked only as redundant metadata and is never dereferenced. */
static ULONG mempool_backing_derive_container(
    MEMPOOL_BACKING *backing,
    MEMPOOL_PAGE **page_out,
    MEMPOOL_LARGE_CHUNK **chunk_out,
    POOL **pool_out)
{
    MEMPOOL_PAGE *page = (MEMPOOL_PAGE *)0;
    MEMPOOL_LARGE_CHUNK *chunk = (MEMPOOL_LARGE_CHUNK *)0;
    ULONG expected_size = 0;

    if (page_out != (MEMPOOL_PAGE **)0)
        *page_out = (MEMPOOL_PAGE *)0;
    if (chunk_out != (MEMPOOL_LARGE_CHUNK **)0)
        *chunk_out = (MEMPOOL_LARGE_CHUNK *)0;
    if (pool_out != (POOL **)0)
        *pool_out = (POOL *)0;
    if (backing == (MEMPOOL_BACKING *)0)
        return MEMPOOL_ABEND_ALLOCATION_METADATA;

    if (backing->kind == MEMPOOL_BACKING_PAGE) {
        page = (MEMPOOL_PAGE *)((UCHAR *)backing -
                                offsetof(MEMPOOL_PAGE, backing));
        if (backing != &page->backing || backing->object != page ||
            backing->base != page ||
            ((ULONG_PTR)page & MEMPOOL_PAGE_MASK) != 0 ||
            backing->size != MEMPOOL_PAGE_SIZE ||
            backing->tag != page->eyecatcher ||
            page->pool == (POOL *)0)
            return MEMPOOL_ABEND_FREE_CELLS_HEADER;
        if (page_out != (MEMPOOL_PAGE **)0)
            *page_out = page;
        if (pool_out != (POOL **)0)
            *pool_out = page->pool;
        return 0;
    }

    if (backing->kind == MEMPOOL_BACKING_LARGE) {
        chunk = (MEMPOOL_LARGE_CHUNK *)((UCHAR *)backing -
                offsetof(MEMPOOL_LARGE_CHUNK, backing));
        if (backing != &chunk->backing || backing->object != chunk ||
            backing->base != chunk->ptr || chunk->ptr == (void *)0 ||
            ((ULONG_PTR)chunk->ptr & MEMPOOL_PAGE_MASK) != 0 ||
            backing->tag != chunk->eyecatcher || chunk->pool == (POOL *)0 ||
            chunk->allocation_size <= MEMPOOL_LARGE_CHUNK_MINIMUM ||
            chunk->allocation_size > MEMPOOL_LARGE_CHUNK_MAXIMUM)
            return MEMPOOL_ABEND_LARGE_HEADER;
        expected_size =
            (chunk->allocation_size + MEMPOOL_PAGE_SIZE - 1UL) &
            ~(MEMPOOL_PAGE_SIZE - 1UL);
        if (expected_size == 0 || backing->size != expected_size ||
            chunk->size != expected_size)
            return MEMPOOL_ABEND_LARGE_HEADER;
        if (chunk_out != (MEMPOOL_LARGE_CHUNK **)0)
            *chunk_out = chunk;
        if (pool_out != (POOL **)0)
            *pool_out = chunk->pool;
        return 0;
    }
    return MEMPOOL_ABEND_ALLOCATION_METADATA;
}

static int mempool_backing_release_storage(
    MEMPOOL_REGISTRATION *expected_registration,
    POOL *expected_pool,
    MEMPOOL_BACKING *backing)
{
    void *base = (void *)0;
    void *object = (void *)0;
    MEMPOOL_PAGE *page = (MEMPOOL_PAGE *)0;
    MEMPOOL_LARGE_CHUNK *chunk = (MEMPOOL_LARGE_CHUNK *)0;
    POOL *pool = (POOL *)0;
    ULONG reason = 0;
    ULONG tag = 0;

    reason = mempool_backing_derive_container(backing, &page, &chunk, &pool);
    if (reason != 0 ||
        expected_registration == (MEMPOOL_REGISTRATION *)0 ||
        expected_pool == (POOL *)0 || pool != expected_pool ||
        backing->registration != expected_registration ||
        expected_registration->pool != expected_pool ||
        expected_pool->registration != expected_registration ||
        pool->eyecatcher != backing->tag ||
        pool->pool_type != expected_registration->pool_type) {
        mempool_abend(reason != 0 ? reason :
                      MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        return 0;
#endif
    }
    if (page != (MEMPOOL_PAGE *)0) {
        base = page;
        tag = page->eyecatcher;
    } else {
        base = chunk->ptr;
        object = chunk;
        tag = chunk->eyecatcher;
    }
    /* All data used after the backing allocation is released is now local. */
    mempool_free_mem(base, tag);
    if (object != (void *)0)
        mempool_metadata_free(object);
    return 1;
}

static void mempool_backing_put(MEMPOOL_REGISTRATION *registration,
                                POOL *pool,
                                MEMPOOL_BACKING *backing)
{
    LONG remaining = InterlockedDecrement(&backing->references);

    if (remaining < 0) {
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        return;
#endif
    }
    if (remaining == 0 &&
        InterlockedCompareExchange(&backing->closing, 0, 0) != 0)
        (void)mempool_backing_release_storage(registration, pool, backing);
}

static int mempool_registry_add(MEMPOOL_REGISTRATION *registration,
                                MEMPOOL_BACKING *initial_backing)
{
    MEMPOOL_REGISTRATION *existing = (MEMPOOL_REGISTRATION *)0;
    ULONG bucket = 0;
    ULONG bucket_entries = 0;
    int inserted = 0;
    MEMPOOL_REGISTRY_LOCK_DECL

    if (registration == (MEMPOOL_REGISTRATION *)0 ||
        initial_backing == (MEMPOOL_BACKING *)0 ||
        registration->pool == (POOL *)0 ||
        initial_backing->registration != registration ||
        registration->next != (MEMPOOL_REGISTRATION *)0)
        return 0;

    MEMPOOL_REGISTRY_LOCK();
    /* Validate the complete handle bucket before publishing either the
       registration or its initial backing.  Once this succeeds the lock keeps
       the bucket stable, so backing insertion is the last fallible step. */
    if (mempool_registration_counts_full_valid_locked() &&
        mempool_backing_counts_full_valid_locked() &&
        mempool_registration_count < MEMPOOL_REGISTRY_CAPACITY &&
        mempool_registry_lookup_locked(registration->pool, &existing,
                                        (MEMPOOL_REGISTRATION **)0,
                                        &bucket_entries) &&
        existing == (MEMPOOL_REGISTRATION *)0 &&
        bucket_entries < MEMPOOL_REGISTRY_BUCKET_LIMIT)
        inserted = mempool_backing_insert_locked(initial_backing);
    if (inserted != 0) {
        bucket = mempool_pointer_bucket(registration->pool);
        registration->next = mempool_registry[bucket];
        mempool_registry[bucket] = registration;
        ++mempool_registration_bucket_counts[bucket];
        --mempool_registration_bucket_checks[bucket];
        ++mempool_registration_count;
        --mempool_registration_count_check;
    }
    MEMPOOL_REGISTRY_UNLOCK();
    return inserted;
}

#if defined(MEMPOOL_TESTING)
int DEVLIB_API_CALL mempool_test_dpc_zero_request_allowed(ULONG size)
{
    return mempool_dpc_zero_request_allowed(size);
}

int DEVLIB_API_CALL mempool_test_registry_snapshot(
    MEMPOOL *pool,
    MEMPOOL_REGISTRY_TEST_SNAPSHOT *snapshot)
{
    MEMPOOL_REGISTRATION *registration = (MEMPOOL_REGISTRATION *)0;
    ULONG bucket = 0;
    int valid = 0;
    MEMPOOL_REGISTRY_LOCK_DECL

    if (pool == (MEMPOOL *)0 ||
        snapshot == (MEMPOOL_REGISTRY_TEST_SNAPSHOT *)0 ||
        !mempool_registry_lock_is_initialized())
        return 0;
    MEMPOOL_REGISTRY_LOCK();
    valid = mempool_registry_lookup_locked(
                (POOL *)pool, &registration,
                (MEMPOOL_REGISTRATION **)0, (ULONG *)0);
    if (valid != 0 && registration != (MEMPOOL_REGISTRATION *)0) {
        bucket = mempool_pointer_bucket(pool);
        snapshot->registration = registration;
        snapshot->registration_next = registration->next;
        snapshot->registration_count = mempool_registration_count;
        snapshot->bucket_count = mempool_registration_bucket_counts[bucket];
        snapshot->bucket = bucket;
    }
    MEMPOOL_REGISTRY_UNLOCK();
    return valid != 0 && registration != (MEMPOOL_REGISTRATION *)0;
}

int DEVLIB_API_CALL mempool_test_registry_corrupt(
    const MEMPOOL_REGISTRY_TEST_SNAPSHOT *snapshot,
    MEMPOOL_REGISTRY_TEST_CORRUPTION corruption)
{
    MEMPOOL_REGISTRATION *registration = (MEMPOOL_REGISTRATION *)0;
    int changed = 0;
    MEMPOOL_REGISTRY_LOCK_DECL

    if (snapshot == (const MEMPOOL_REGISTRY_TEST_SNAPSHOT *)0 ||
        snapshot->registration == (void *)0 ||
        snapshot->bucket >= MEMPOOL_REGISTRY_BUCKET_COUNT ||
        !mempool_registry_lock_is_initialized())
        return 0;
    registration = (MEMPOOL_REGISTRATION *)snapshot->registration;
    MEMPOOL_REGISTRY_LOCK();
    if (registration->next ==
            (MEMPOOL_REGISTRATION *)snapshot->registration_next &&
        mempool_registration_count == snapshot->registration_count &&
        mempool_registration_bucket_counts[snapshot->bucket] ==
            snapshot->bucket_count) {
        if (corruption == MEMPOOL_REGISTRY_TEST_SELF_CYCLE) {
            registration->next = registration;
            changed = 1;
        } else if (corruption == MEMPOOL_REGISTRY_TEST_GLOBAL_COUNT_SMALL &&
                   mempool_registration_count != 0) {
            --mempool_registration_count;
            changed = 1;
        } else if (corruption == MEMPOOL_REGISTRY_TEST_GLOBAL_COUNT_LARGE &&
                   mempool_registration_count != 0xFFFFFFFFUL) {
            ++mempool_registration_count;
            changed = 1;
        } else if (corruption == MEMPOOL_REGISTRY_TEST_BUCKET_COUNT_SMALL &&
                   mempool_registration_bucket_counts[snapshot->bucket] != 0) {
            --mempool_registration_bucket_counts[snapshot->bucket];
            changed = 1;
        } else if (corruption == MEMPOOL_REGISTRY_TEST_BUCKET_COUNT_LARGE &&
                   mempool_registration_bucket_counts[snapshot->bucket] !=
                       0xFFFFFFFFUL) {
            ++mempool_registration_bucket_counts[snapshot->bucket];
            changed = 1;
        }
    }
    MEMPOOL_REGISTRY_UNLOCK();
    return changed;
}

void DEVLIB_API_CALL mempool_test_registry_restore(
    const MEMPOOL_REGISTRY_TEST_SNAPSHOT *snapshot)
{
    MEMPOOL_REGISTRATION *registration = (MEMPOOL_REGISTRATION *)0;
    MEMPOOL_REGISTRY_LOCK_DECL

    if (snapshot == (const MEMPOOL_REGISTRY_TEST_SNAPSHOT *)0 ||
        snapshot->registration == (void *)0 ||
        snapshot->bucket >= MEMPOOL_REGISTRY_BUCKET_COUNT ||
        !mempool_registry_lock_is_initialized())
        return;
    registration = (MEMPOOL_REGISTRATION *)snapshot->registration;
    MEMPOOL_REGISTRY_LOCK();
    registration->next =
        (MEMPOOL_REGISTRATION *)snapshot->registration_next;
    mempool_registration_count = snapshot->registration_count;
    mempool_registration_bucket_counts[snapshot->bucket] =
        snapshot->bucket_count;
    MEMPOOL_REGISTRY_UNLOCK();
}
#endif

static MEMPOOL_REGISTRATION *mempool_operation_admit(POOL *pool)
{
    MEMPOOL_REGISTRATION *registration = (MEMPOOL_REGISTRATION *)0;
    MEMPOOL_REGISTRATION *accepted = (MEMPOOL_REGISTRATION *)0;
    int registry_valid = 0;
    MEMPOOL_REGISTRY_LOCK_DECL
#ifdef _KERNEL_MODE
    KIRQL current_irql = PASSIVE_LEVEL;
#endif

    if (pool == (POOL *)0)
        return (MEMPOOL_REGISTRATION *)0;
#ifdef _KERNEL_MODE
    current_irql = KeGetCurrentIrql();
    if (current_irql > DISPATCH_LEVEL)
        return (MEMPOOL_REGISTRATION *)0;
#endif
    /* Before the first Create there cannot be a valid opaque handle.  Avoid
       acquiring (or racing initialization of) the process-wide spin lock for
       such invalid early calls. */
    if (!mempool_registry_lock_is_initialized())
        return (MEMPOOL_REGISTRATION *)0;
    MEMPOOL_REGISTRY_LOCK();
    registry_valid = mempool_registry_lookup_locked(
                         pool, &registration,
                         (MEMPOOL_REGISTRATION **)0, (ULONG *)0);
    if (registry_valid != 0 &&
        registration != (MEMPOOL_REGISTRATION *)0) {
#ifdef _KERNEL_MODE
        if (current_irql == DISPATCH_LEVEL &&
            registration->pool_type == (ULONG)MEMPOOL_PAGED)
            registration = (MEMPOOL_REGISTRATION *)0;
#endif
        if (registration != (MEMPOOL_REGISTRATION *)0 &&
            mempool_operation_enter(registration))
            accepted = registration;
    }
    MEMPOOL_REGISTRY_UNLOCK();
    return accepted;
}

static void mempool_operation_leave(MEMPOOL_REGISTRATION *registration)
{
    (void)InterlockedDecrement(&registration->lifecycle_state);
}

static int mempool_begin_destroy(MEMPOOL_REGISTRATION *registration)
{
    LONG state = 0;
    LONG desired = 0;

    if (registration == (MEMPOOL_REGISTRATION *)0)
        return 0;
    for (;;) {
        state = InterlockedCompareExchange(&registration->lifecycle_state,
                                           0, 0);
        if ((state & MEMPOOL_LIFECYCLE_CLOSING) != 0)
            return 0;
        desired = state | MEMPOOL_LIFECYCLE_CLOSING;
        if (InterlockedCompareExchange(&registration->lifecycle_state,
                                       desired, state) == state)
            break;
    }
    return 1;
}

static void mempool_wait_for_operations(MEMPOOL_REGISTRATION *registration)
{
    while ((InterlockedCompareExchange(&registration->lifecycle_state,
                                       0, 0) &
            MEMPOOL_LIFECYCLE_COUNT_MASK) != 0)
        mempool_passive_wait();
}

/* ------------------------------------------------------------------------- */
/* Backing memory and page management                                       */
/* ------------------------------------------------------------------------- */

static void *mempool_alloc_mem(ULONG pool_type, ULONG size, ULONG tag)
{
    void *address = (void *)0;
#ifndef _KERNEL_MODE
    SIZE_T region_size = 0;
    DWORD protection = 0;
#endif
#ifdef _KERNEL_MODE
    /* A KSPIN_LOCK protects page metadata at raised IRQL, so all kernel
       backing pages must be resident even when the logical pool type is
       MEMPOOL_PAGED.  Mempool_Alloc still rejects that logical type at DPC. */
    UNREFERENCED_PARAMETER(pool_type);
    address = Allocator_Malloc(TRUE, (size_t)size, tag);
#else
    /* VirtualAlloc supplies the 64 KiB alignment required by user mode. */
    UNREFERENCED_PARAMETER(pool_type);
    region_size = (SIZE_T)size;
    /* Tag 0xFF retains the original executable-memory use case. */
    protection = ((UCHAR)tag == (UCHAR)0xFFU) ?
                 PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    address = VirtualAlloc((LPVOID)0, region_size,
                           MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN,
                           protection);
#endif
    return address;
}

static void mempool_free_mem(void *address, ULONG tag)
{
#ifndef _KERNEL_MODE
    BOOL result = FALSE;
#endif

    if (address == (void *)0)
        return;
#ifdef _KERNEL_MODE
    /* The tag must match the tag used by the corresponding allocation. */
    Allocator_Free(address, tag);
#else
    UNREFERENCED_PARAMETER(tag);
    result = VirtualFree(address, 0, MEM_RELEASE);
    if (!result)
        mempool_abend(MEMPOOL_ABEND_FREE_SIZE_MISMATCH);
#endif
}

static void *mempool_metadata_alloc(SIZE_T size)
{
#ifdef _KERNEL_MODE
    return Allocator_Malloc((BOOLEAN)1, (size_t)size, MEMPOOL_POOL_TAG);
#else
    return Allocator_Malloc((size_t)size);
#endif
}

static void mempool_metadata_free(void *address)
{
    if (address == (void *)0)
        return;
#ifdef _KERNEL_MODE
    Allocator_Free(address, MEMPOOL_POOL_TAG);
#else
    Allocator_Free(address);
#endif
}

static MEMPOOL_PAGE *mempool_alloc_page(POOL *pool, ULONG pool_type, ULONG tag)
{
    MEMPOOL_PAGE *page = (MEMPOOL_PAGE *)0;
    UCHAR *bitmap = (UCHAR *)0;
    MEMPOOL_CELL_COUNT_ENTRY *lengths = (MEMPOOL_CELL_COUNT_ENTRY *)0;

    /* A page is always obtained as one aligned OS allocation. */
    page = (MEMPOOL_PAGE *)mempool_alloc_mem(pool_type, MEMPOOL_PAGE_SIZE, tag);
    if (page == (MEMPOOL_PAGE *)0)
        return (MEMPOOL_PAGE *)0;
    page->eyecatcher = tag;
    page->next = (MEMPOOL_PAGE *)0;
    page->num_free = (USHORT)MEMPOOL_NUM_PAGE_CELLS;
    page->num_used = 0;
    page->max_free_run = (USHORT)MEMPOOL_NUM_PAGE_CELLS;
    page->pool = pool;
    page->list_elem.next = (LIST_ELEM *)0;
    page->list_elem.prev = (LIST_ELEM *)0;
    page->bin_next = (MEMPOOL_PAGE *)0;
    page->bin_prev = (MEMPOOL_PAGE *)0;
    if (pool != (POOL *)0) {
        mempool_backing_prepare(&page->backing, pool->registration,
                                page, page, MEMPOOL_PAGE_SIZE, tag,
                                MEMPOOL_BACKING_PAGE);
        /* New pages inherit the bits that are permanently unavailable for
           cells (bitmap padding).  The first page additionally marks POOL
           cells after the pool object has been initialized. */
        bitmap = (UCHAR *)page + MEMPOOL_PAGE_HEADER_SIZE;
        mempool_copy(bitmap, pool->initial_bitmap,
                     (SIZE_T)MEMPOOL_PAGE_BITMAP_SIZE);
        lengths = (MEMPOOL_CELL_COUNT_ENTRY *)(bitmap +
                                               MEMPOOL_PAGE_BITMAP_SIZE);
        mempool_zero(lengths, (SIZE_T)MEMPOOL_PAGE_LENGTHS_SIZE);
    }
    return page;
}

/* ------------------------------------------------------------------------- */
/* Pool lifetime and public allocation API                                  */
/* ------------------------------------------------------------------------- */

static POOL *mempool_create(ULONG pool_type, ULONG tag)
{
    MEMPOOL_PAGE *page = (MEMPOOL_PAGE *)0;
    UCHAR *bitmap = (UCHAR *)0;
    MEMPOOL_CELL_COUNT_ENTRY *lengths = (MEMPOOL_CELL_COUNT_ENTRY *)0;
    POOL *pool = (POOL *)0;
    ULONG i = 0;
    ULONG byte_index = 0;
    UCHAR bit = 0;
    ULONG pool_cells = 0;

    /* Step 1: allocate the page that will contain both the pool and data. */
    page = mempool_alloc_page((POOL *)0, pool_type, tag);
    if (page == (MEMPOOL_PAGE *)0)
        return (POOL *)0;

    /* Step 2: clear the bitmap and reserve bits beyond the real cell count. */
    bitmap = (UCHAR *)page + MEMPOOL_PAGE_HEADER_SIZE;
    mempool_zero(bitmap, (SIZE_T)MEMPOOL_PAGE_BITMAP_SIZE);
    i = MEMPOOL_NUM_PAGE_CELLS;
    while (i < MEMPOOL_PAGE_BITMAP_SIZE * 8UL) {
        byte_index = i / 8UL;
        bit = (UCHAR)(1U << (i & 7UL));
        bitmap[byte_index] |= bit;
        ++i;
    }

    lengths = (MEMPOOL_CELL_COUNT_ENTRY *)(bitmap +
                                           MEMPOOL_PAGE_BITMAP_SIZE);
    mempool_zero(lengths, (SIZE_T)MEMPOOL_PAGE_LENGTHS_SIZE);

    /* Step 3: place the pool object at the beginning of the data area. */
    pool = (POOL *)((UCHAR *)page + MEMPOOL_PAGE_DATA_OFFSET);
    page->pool = pool;
    mempool_zero(pool, (SIZE_T)sizeof(POOL));
    pool->eyecatcher = tag;
    pool->pool_type = pool_type;
    pool->cookie_seed = (ULONG)((ULONG_PTR)pool ^
                        ((ULONG_PTR)page >> 4) ^ (ULONG_PTR)tag ^
                        (ULONG_PTR)0xA5C39E17UL);
    if (pool->cookie_seed == 0)
        pool->cookie_seed = (ULONG)0x6D2B79F5UL;
#ifdef _KERNEL_MODE
    KeInitializeSpinLock(&pool->spin_lock);
#else
    if (!InitializeCriticalSectionAndSpinCount(&pool->lock, 1000UL)) {
        mempool_free_mem(page, tag);
        return (POOL *)0;
    }
#endif

    /* Step 4: save the template used when later pages are created. */
    mempool_copy(pool->initial_bitmap, bitmap,
                 (SIZE_T)MEMPOOL_PAGE_BITMAP_SIZE);
    List_Init(&pool->pages);
    List_Init(&pool->full_pages);
    List_Init(&pool->large_chunks);

    /* Step 5: reserve the cells occupied by POOL in the first page. */
    pool_cells = MEMPOOL_NUM_CELLS(sizeof(POOL));
    if (pool_cells > MEMPOOL_NUM_PAGE_CELLS) {
#ifndef _KERNEL_MODE
        DeleteCriticalSection(&pool->lock);
#endif
        mempool_free_mem(page, tag);
        return (POOL *)0;
    }
    i = 0;
    while (i < pool_cells) {
        byte_index = i / 8UL;
        bit = (UCHAR)(1U << (i & 7UL));
        bitmap[byte_index] |= bit;
        ++i;
    }
    page->num_free = (USHORT)(MEMPOOL_NUM_PAGE_CELLS - pool_cells);
    page->num_used = (USHORT)pool_cells;
    page->max_free_run = (USHORT)(MEMPOOL_NUM_PAGE_CELLS - pool_cells);

    /* This private initial page is linked before the registration becomes
       visible.  All later structural changes use identity-validated helpers. */
    pool->pages.head = (LIST_ELEM *)page;
    pool->pages.tail = (LIST_ELEM *)page;
    pool->pages.count = 1;
    pool->page_count = 1;
    pool->page_count_check = (ULONG)0 - (ULONG)1;
    pool->run_bins[page->max_free_run] = page;
    pool->run_bin_counts[page->max_free_run] = (USHORT)1;
    pool->run_bin_checks[page->max_free_run] = (USHORT)0xFFFFU;
    return pool;
}

/* Validate side metadata before Destroy unpublishes a backing.  Returning a
 * reason instead of reporting here lets the caller fail closed in user mode
 * when a debugger continues after DebugBreak. */
static ULONG mempool_validate_destroy_backing(
    POOL *pool,
    MEMPOOL_REGISTRATION *registration,
    MEMPOOL_BACKING *backing)
{
    MEMPOOL_ALLOCATION_HEADER *allocation_header =
        (MEMPOOL_ALLOCATION_HEADER *)0;
    MEMPOOL_LARGE_CHUNK *large_chunk = (MEMPOOL_LARGE_CHUNK *)0;
    MEMPOOL_PAGE *page = (MEMPOOL_PAGE *)0;
    POOL *derived_pool = (POOL *)0;
    ULONG reason = 0;

    if (pool == (POOL *)0 || registration == (MEMPOOL_REGISTRATION *)0 ||
        backing == (MEMPOOL_BACKING *)0)
        return MEMPOOL_ABEND_ALLOCATION_METADATA;
    reason = mempool_backing_derive_container(backing, &page, &large_chunk,
                                               &derived_pool);
    if (reason != 0)
        return reason;
    if (derived_pool != pool || backing->registration != registration ||
        registration->pool != pool || pool->registration != registration ||
        pool->pool_type != registration->pool_type ||
        pool->eyecatcher != backing->tag)
        return MEMPOOL_ABEND_ALLOCATION_METADATA;

    if (large_chunk != (MEMPOOL_LARGE_CHUNK *)0) {
        if (large_chunk->eyecatcher != pool->eyecatcher)
            return MEMPOOL_ABEND_LARGE_HEADER;
        allocation_header =
            (MEMPOOL_ALLOCATION_HEADER *)backing->base;
        if (allocation_header->pool != pool ||
            allocation_header->total_size != large_chunk->allocation_size ||
            allocation_header->cookie != large_chunk->cookie ||
            large_chunk->cookie != mempool_allocation_cookie(
                pool, backing->base, large_chunk->allocation_size))
            return MEMPOOL_ABEND_ALLOCATION_METADATA;
        return 0;
    }

    if (page != (MEMPOOL_PAGE *)0) {
        if (page->eyecatcher != pool->eyecatcher)
            return MEMPOOL_ABEND_FREE_CELLS_HEADER;
        return 0;
    }
    return MEMPOOL_ABEND_ALLOCATION_METADATA;
}

/*
 * Destroy performs this complete, read-only pass before retiring the first
 * backing.  The registration has already been closed and unpublished and all
 * admitted operations have drained, so its owner chain cannot legitimately
 * change while the process-wide hash is checked one node at a time.  Keeping
 * each registry-lock hold short avoids blocking DPC-level work in other pools.
 */
static ULONG mempool_preflight_destroy(
    POOL *pool,
    MEMPOOL_REGISTRATION *registration,
    MEMPOOL_BACKING *pool_backing)
{
    MEMPOOL_BACKING *backing = (MEMPOOL_BACKING *)0;
    ULONG count = 0;
    ULONG index = 0;
    ULONG reason = 0;
    int base_hash_valid = 0;
    int identity_hash_valid = 0;
    MEMPOOL_REGISTRY_LOCK_DECL

    if (pool == (POOL *)0 || registration == (MEMPOOL_REGISTRATION *)0 ||
        pool_backing == (MEMPOOL_BACKING *)0)
        return MEMPOOL_ABEND_ALLOCATION_METADATA;
    count = registration->backing_count;
    if (count == 0 ||
        !mempool_backing_owner_validate(registration, pool_backing,
                                        (MEMPOOL_BACKING **)0))
        return MEMPOOL_ABEND_ALLOCATION_METADATA;

    backing = registration->backings;
    while (index < count) {
        /* owner_validate proved this exact traversal is finite and complete. */
        if (InterlockedCompareExchange(&backing->closing, 0, 0) != 0 ||
            InterlockedCompareExchange(&backing->references, 0, 0) != 1)
            return MEMPOOL_ABEND_ALLOCATION_METADATA;
        reason = mempool_validate_destroy_backing(pool, registration,
                                                   backing);
        if (reason != 0)
            return reason;

        MEMPOOL_REGISTRY_LOCK();
        base_hash_valid = mempool_backing_base_bucket_validate_locked(
                              mempool_pointer_bucket(backing->base), backing,
                              (MEMPOOL_BACKING **)0);
        identity_hash_valid =
            mempool_backing_identity_bucket_validate_locked(
                mempool_identity_bucket(backing), backing,
                (MEMPOOL_BACKING **)0);
        MEMPOOL_REGISTRY_UNLOCK();
        if (base_hash_valid == 0 || identity_hash_valid == 0)
            return MEMPOOL_ABEND_ALLOCATION_METADATA;
        backing = backing->owner_next;
        ++index;
    }
    return backing == (MEMPOOL_BACKING *)0 ? 0 :
           MEMPOOL_ABEND_ALLOCATION_METADATA;
}

#ifdef _KERNEL_MODE
_Use_decl_annotations_
#endif
MEMPOOL *DEVLIB_API_CALL Mempool_CreatePool(MEMPOOL_TYPE type)
{
    MEMPOOL_REGISTRATION *registration = (MEMPOOL_REGISTRATION *)0;
    MEMPOOL_PAGE *pool_page = (MEMPOOL_PAGE *)0;
    POOL *pool = (POOL *)0;

    /* Keep the public type check here so invalid enum values never reach the
       platform-specific allocation routine. */
    if (type != MEMPOOL_PAGED && type != MEMPOOL_NONPAGED)
        return (MEMPOOL *)0;
#ifdef _KERNEL_MODE
    /* Creation allocates registry and page metadata, so keep it passive. */
    if (KeGetCurrentIrql() >= APC_LEVEL)
        return (MEMPOOL *)0;
#endif
    mempool_registry_lock_initialize();
    /* The separate registration lets admission validate a currently live
       opaque handle by address comparison without dereferencing it. */
    registration = (MEMPOOL_REGISTRATION *)mempool_registry_alloc();
    if (registration == (MEMPOOL_REGISTRATION *)0)
        return (MEMPOOL *)0;
    pool = mempool_create((ULONG)type, MEMPOOL_POOL_TAG);
    if (pool == (POOL *)0) {
        mempool_registry_free(registration);
        return (MEMPOOL *)0;
    }
    registration->pool = pool;
    registration->pool_type = (ULONG)type;
    registration->lifecycle_state = 0;
    registration->backings = (MEMPOOL_BACKING *)0;
    registration->backing_count = 0;
    registration->next = (MEMPOOL_REGISTRATION *)0;
    pool->registration = registration;
    pool_page = (MEMPOOL_PAGE *)((ULONG_PTR)pool & ~MEMPOOL_PAGE_MASK);
    mempool_backing_prepare(&pool_page->backing, registration, pool_page,
                            pool_page, MEMPOOL_PAGE_SIZE, MEMPOOL_POOL_TAG,
                            MEMPOOL_BACKING_PAGE);
    if (!mempool_registry_add(registration, &pool_page->backing)) {
#ifndef _KERNEL_MODE
        DeleteCriticalSection(&pool->lock);
#endif
        mempool_free_mem(pool_page, MEMPOOL_POOL_TAG);
        mempool_registry_free(registration);
        return (MEMPOOL *)0;
    }
    return pool;
}

#ifdef _KERNEL_MODE
_Use_decl_annotations_
#endif
ULONG DEVLIB_API_CALL Mempool_DestroyPool(MEMPOOL *pool)
{
    MEMPOOL_BACKING *backing = (MEMPOOL_BACKING *)0;
    MEMPOOL_BACKING *pool_backing = (MEMPOOL_BACKING *)0;
    MEMPOOL_PAGE *pool_page = (MEMPOOL_PAGE *)0;
    LONG backing_references = -1;
    ULONG backing_page_count = 0;
    ULONG page_count = 0;
    ULONG validation_reason = 0;
    MEMPOOL_REGISTRATION *registration = (MEMPOOL_REGISTRATION *)0;
    MEMPOOL_REGISTRATION *registration_previous =
        (MEMPOOL_REGISTRATION *)0;
    int remove_result = 0;
    int registry_valid = 0;
    MEMPOOL_REGISTRY_LOCK_DECL
#ifdef _KERNEL_MODE
    KIRQL current_irql = PASSIVE_LEVEL;
#endif

    /* Unlink under the short registry lock, then drain this pool only. */
    if (pool == (POOL *)0)
        return 0;
#ifdef _KERNEL_MODE
    /* Destruction waits for admitted operations; an APC/DPC must not wait for
       a lower-IRQL owner on the same CPU, even for a non-paged pool. */
    current_irql = KeGetCurrentIrql();
    if (current_irql >= APC_LEVEL)
        return 0;
#endif
    if (!mempool_registry_lock_is_initialized())
        return 0;
    MEMPOOL_REGISTRY_LOCK();
    if (!mempool_registration_counts_full_valid_locked()) {
        MEMPOOL_REGISTRY_UNLOCK();
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        return 0;
#endif
    }
    registry_valid = mempool_registry_lookup_locked(
                         pool, &registration,
                         &registration_previous, (ULONG *)0);
    if (registry_valid == 0) {
        MEMPOOL_REGISTRY_UNLOCK();
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        return 0;
#endif
    }
    if (registration == (MEMPOOL_REGISTRATION *)0) {
        MEMPOOL_REGISTRY_UNLOCK();
        return 0;
    }
    remove_result = mempool_registry_close_remove_locked(
                        pool, registration, registration_previous);
    if (remove_result == 0) {
        MEMPOOL_REGISTRY_UNLOCK();
        return 0;
    }
    if (remove_result < 0) {
        MEMPOOL_REGISTRY_UNLOCK();
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        return 0;
#endif
    }
    MEMPOOL_REGISTRY_UNLOCK();
    mempool_wait_for_operations(registration);
    pool_page = (MEMPOOL_PAGE *)((ULONG_PTR)pool & ~MEMPOOL_PAGE_MASK);
    pool_backing = &pool_page->backing;
    validation_reason = mempool_preflight_destroy(pool, registration,
                                                   pool_backing);
    if (validation_reason != 0) {
        /* Strict fail-closed quarantine: admission and the handle registry stay
           closed, while every backing index/reference/storage byte is left
           untouched for diagnosis. */
        mempool_abend(validation_reason);
#ifndef _KERNEL_MODE
        return 0;
#endif
    }

    page_count = 0;
    /* The owner chain is authoritative, including a backing published while
       an admitted allocator was outside the pool lock.  Release the page that
       contains POOL last because it owns the lock and list heads. */
    while (registration->backing_count > 1) {
        backing = registration->backings;
        if (backing == pool_backing)
            backing = backing->owner_next;
        if (backing == (MEMPOOL_BACKING *)0 || backing == pool_backing)
            break;
        backing_page_count = backing->size / MEMPOOL_PAGE_SIZE;
        backing_references = mempool_backing_retire_prevalidated(
                                 registration, backing);
        if (backing_references != 0) {
            mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
            return page_count;
#endif
        }
        if (!mempool_backing_release_storage(registration, pool, backing)) {
#ifndef _KERNEL_MODE
            return page_count;
#endif
        }
        if (backing_page_count > 0xFFFFFFFFUL - page_count)
            page_count = 0xFFFFFFFFUL;
        else
            page_count += backing_page_count;
    }

    if (registration->backings != pool_backing ||
        registration->backing_count != 1) {
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        return page_count;
#endif
    }
    backing_references = mempool_backing_retire_prevalidated(registration,
                                                              pool_backing);
    if (backing_references != 0) {
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        return page_count;
#endif
    }
    if (page_count != 0xFFFFFFFFUL)
        ++page_count;

#ifdef _KERNEL_MODE
    /* KSPIN_LOCK is embedded in the pool and needs no separate cleanup. */
#else
    DeleteCriticalSection(&pool->lock);
#endif
    if (!mempool_backing_release_storage(registration, pool,
                                         &pool_page->backing)) {
#ifndef _KERNEL_MODE
        return page_count;
#endif
    }
    /* It was unlinked before the drain, so no new caller can retain it. */
    mempool_registry_free(registration);
    return page_count;
}

#ifdef _KERNEL_MODE
_Use_decl_annotations_
#endif
void *DEVLIB_API_CALL Mempool_Alloc(MEMPOOL *pool, ULONG size)
{
    MEMPOOL_ALLOCATION_HEADER *header = (MEMPOOL_ALLOCATION_HEADER *)0;
    MEMPOOL_REGISTRATION *registration = (MEMPOOL_REGISTRATION *)0;
    void *address = (void *)0;
    void *payload = (void *)0;
    ULONG total_size = 0;
    ULONG cell_count = 0;
#if MEMPOOL_ZERO_ON_ALLOC
    SIZE_T clear_size = 0;
#endif

    /* The aligned hidden header preserves 8-byte x86 and 16-byte x64/ARM64
       payload alignment while binding size and owner with a cookie. */
    if (pool == (POOL *)0 || size == 0)
        return (void *)0;
    /* The hidden size word is part of every internal allocation. */
    /* Reject requests that could overflow either the hidden size word or
       the later page-rounding arithmetic. */
    if (size > MEMPOOL_MAX_ALLOC_SIZE)
        return (void *)0;
    registration = mempool_operation_admit(pool);
    if (registration == (MEMPOOL_REGISTRATION *)0) {
        return (void *)0;
    }
    if (!mempool_current_irql_allows_pool(pool)) {
        mempool_operation_leave(registration);
        return (void *)0;
    }
#if defined(_KERNEL_MODE) && MEMPOOL_ZERO_ON_ALLOC
    /* Reject excessive synchronous clearing at DPC before allocating or
       publishing any backing.  Admission itself is reversible and is released
       on this path.  APC/PASSIVE calls retain the configured zeroing policy. */
    if (KeGetCurrentIrql() == DISPATCH_LEVEL &&
        !mempool_dpc_zero_request_allowed(size)) {
        mempool_operation_leave(registration);
        return (void *)0;
    }
#endif
    total_size = size + (ULONG)MEMPOOL_ALLOCATION_HEADER_SIZE;
#if MEMPOOL_ZERO_ON_ALLOC
    clear_size = mempool_zero_clear_size(size);
#endif
    /* A large request bypasses the bitmap and receives page-granular memory. */
    if (total_size > MEMPOOL_LARGE_CHUNK_MINIMUM) {
        address = mempool_get_large_chunk(pool, total_size);
    } else {
        cell_count = MEMPOOL_NUM_CELLS(total_size);
        address = mempool_get_cells(pool, cell_count);
    }
    if (address != (void *)0) {
        header = (MEMPOOL_ALLOCATION_HEADER *)address;
        header->total_size = total_size;
        header->pool = pool;
        header->cookie = mempool_allocation_cookie(pool, address, total_size);
        payload = (UCHAR *)address + MEMPOOL_ALLOCATION_HEADER_SIZE;
#if MEMPOOL_ZERO_ON_ALLOC
        /* Clear the complete rounded capacity, not only the requested prefix:
           otherwise a later larger allocation could inherit a former
           allocation's tail bytes from the same cell run. */
        mempool_zero(payload, clear_size);
#endif
    }
    mempool_operation_leave(registration);
    address = payload;
    return address;
}

#ifdef _KERNEL_MODE
_Use_decl_annotations_
#endif
void DEVLIB_API_CALL Mempool_Free(void *address)
{
    MEMPOOL_BACKING *backing = (MEMPOOL_BACKING *)0;
    MEMPOOL_ALLOCATION_HEADER *header = (MEMPOOL_ALLOCATION_HEADER *)0;
    MEMPOOL_REGISTRATION *registration = (MEMPOOL_REGISTRATION *)0;
    MEMPOOL_PAGE *page = (MEMPOOL_PAGE *)0;
    MEMPOOL_LARGE_CHUNK *chunk = (MEMPOOL_LARGE_CHUNK *)0;
    ULONG total_size = 0;
    void *base_address = (void *)0;
    void *lookup_base = (void *)0;
    POOL *pool = (POOL *)0;
    ULONG backing_kind = 0;
    ULONG validation_reason = MEMPOOL_ABEND_FREE_CELLS_HEADER;
    ULONG_PTR base_value = (ULONG_PTR)0;
    ULONG_PTR backing_value = (ULONG_PTR)0;
    int address_valid = 0;
    int admission_closed = 0;
    int registry_valid = 0;
    MEMPOOL_REGISTRY_LOCK_DECL
#ifdef _KERNEL_MODE
    KIRQL current_irql = PASSIVE_LEVEL;
#endif

    if (address == (void *)0) {
        mempool_abend(MEMPOOL_ABEND_FREE_NULL);
        return;
    }
#ifdef _KERNEL_MODE
    /* Do not even inspect the hidden owner word above DISPATCH_LEVEL. */
    current_irql = KeGetCurrentIrql();
    if (current_irql > DISPATCH_LEVEL)
        return;
#endif
    if (!mempool_registry_lock_is_initialized()) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_HEADER);
        return;
    }
    if ((ULONG_PTR)address < (ULONG_PTR)MEMPOOL_ALLOCATION_HEADER_SIZE) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_RANGE);
        return;
    }
    /* Do not dereference caller-adjacent memory until its resident backing and
       registration have both been pinned from the global hash. */
    base_value = (ULONG_PTR)address - (ULONG_PTR)MEMPOOL_ALLOCATION_HEADER_SIZE;
    base_address = (void *)base_value;
    lookup_base = (void *)(base_value & ~MEMPOOL_PAGE_MASK);
    MEMPOOL_REGISTRY_LOCK();
    backing = mempool_backing_find_locked(lookup_base);
    if (backing != (MEMPOOL_BACKING *)0) {
        validation_reason = mempool_backing_derive_container(
                                backing, &page, &chunk, &pool);
        if (validation_reason == 0) {
            /* Find registration by the derived owner's pointer value before
               dereferencing backing->registration.  A forged object or owner
               pointer therefore fails as metadata instead of becoming an AV. */
            registry_valid = mempool_registry_lookup_locked(
                                 pool, &registration,
                                 (MEMPOOL_REGISTRATION **)0, (ULONG *)0);
            if (registry_valid == 0 ||
                registration == (MEMPOOL_REGISTRATION *)0 ||
                backing->registration != registration ||
                registration->pool != pool ||
                pool->registration != registration ||
                pool->pool_type != registration->pool_type ||
                pool->eyecatcher != backing->tag) {
                validation_reason = MEMPOOL_ABEND_ALLOCATION_METADATA;
                registration = (MEMPOOL_REGISTRATION *)0;
            }
        }
        if (validation_reason == 0) {
            backing_kind = backing->kind;
            backing_value = (ULONG_PTR)backing->base;
            if (backing_kind == MEMPOOL_BACKING_LARGE) {
                if (chunk->cookie != mempool_allocation_cookie(
                                         pool, backing->base,
                                         chunk->allocation_size))
                    validation_reason = MEMPOOL_ABEND_ALLOCATION_METADATA;
                else
                    address_valid = base_value == backing_value;
            } else if (backing_kind == MEMPOOL_BACKING_PAGE &&
                       base_value >=
                           backing_value + MEMPOOL_PAGE_DATA_OFFSET &&
                       base_value < backing_value + MEMPOOL_PAGE_SIZE &&
                       ((base_value - backing_value -
                         MEMPOOL_PAGE_DATA_OFFSET) % MEMPOOL_CELL_SIZE) == 0) {
                address_valid = 1;
            }
        }
        if (validation_reason == 0 && address_valid == 0)
            validation_reason = MEMPOOL_ABEND_FREE_CELLS_RANGE;
    }
#ifdef _KERNEL_MODE
    if (registration != (MEMPOOL_REGISTRATION *)0 &&
        current_irql == DISPATCH_LEVEL &&
        registration->pool_type == (ULONG)MEMPOOL_PAGED) {
        MEMPOOL_REGISTRY_UNLOCK();
        return;
    }
#endif
    if (validation_reason == 0 &&
        registration != (MEMPOOL_REGISTRATION *)0 && address_valid != 0) {
        if (mempool_operation_enter(registration)) {
            (void)InterlockedIncrement(&backing->references);
        } else {
            admission_closed = 1;
            backing = (MEMPOOL_BACKING *)0;
        }
    } else {
        backing = (MEMPOOL_BACKING *)0;
    }
    if (backing == (MEMPOOL_BACKING *)0) {
        MEMPOOL_REGISTRY_UNLOCK();
        if (admission_closed != 0)
            return;
        mempool_abend(validation_reason);
        return;
    }
    MEMPOOL_REGISTRY_UNLOCK();
    if (!mempool_current_irql_allows_pool(pool)) {
        mempool_backing_put(registration, pool, backing);
        mempool_operation_leave(registration);
        return;
    }
    header = (MEMPOOL_ALLOCATION_HEADER *)base_address;
    total_size = header->total_size;
    if (total_size < (ULONG)MEMPOOL_ALLOCATION_HEADER_SIZE ||
        total_size > MEMPOOL_MAX_ALLOC_SIZE +
                     (ULONG)MEMPOOL_ALLOCATION_HEADER_SIZE ||
        header->pool != pool ||
        header->cookie != mempool_allocation_cookie(pool, base_address,
                                                    total_size)) {
        mempool_abend(MEMPOOL_ABEND_FREE_SIZE_MISMATCH);
#ifndef _KERNEL_MODE
        /* User-mode DebugBreak may be continued by a debugger. */
        mempool_backing_put(registration, pool, backing);
        mempool_operation_leave(registration);
        return;
#endif
    }
    if (backing_kind == MEMPOOL_BACKING_PAGE &&
        (page->pool != pool || page->eyecatcher != pool->eyecatcher)) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_HEADER);
#ifndef _KERNEL_MODE
        mempool_backing_put(registration, pool, backing);
        mempool_operation_leave(registration);
        return;
#endif
    }
    if (backing_kind == MEMPOOL_BACKING_LARGE)
        mempool_free_large_chunk(chunk, base_address, total_size);
    else
        mempool_free_cells(page, base_address, total_size);
    mempool_backing_put(registration, pool, backing);
    mempool_operation_leave(registration);
}

/* ------------------------------------------------------------------------- */
/* Cell allocator                                                            */
/* ------------------------------------------------------------------------- */

static ULONG mempool_find_cells(MEMPOOL_PAGE *page, ULONG cell_count,
                                ULONG *largest_run, ULONG *free_cells)
{
    UCHAR *bitmap = (UCHAR *)0;
    ULONG i = 0;
    ULONG run_start = 0;
    ULONG run_length = 0;
    ULONG maximum = 0;
    ULONG free_count = 0;
    ULONG byte_index = 0;
    UCHAR bit_mask = 0;
    UCHAR byte_value = 0;
    ULONG first = (ULONG)-1;

    /*
     * Find the first run of cell_count zero bits and always finish the bounded
     * bitmap scan so largest_run is exact.  A zero bit is free and a one bit
     * is either allocated or permanently reserved.  The exact result is
     * checked against the page's bin before any state is changed.  Full
     * 0x00/0xFF bytes are still handled in one step.
     *
     *     bitmap byte:  00000000  -> skip 8 free cells
     *                   11111111  -> skip 8 occupied cells
     *
     * The final byte can contain padding bits, but those bits are marked
     * occupied during pool creation, so skipping it is safe.
     */
    bitmap = (UCHAR *)page + MEMPOOL_PAGE_HEADER_SIZE;
    while (i < MEMPOOL_NUM_PAGE_CELLS) {
        byte_index = i / 8UL;
        if ((i & 7UL) == 0 &&
            i + 8UL <= MEMPOOL_NUM_PAGE_CELLS) {
            byte_value = bitmap[byte_index];
            if (byte_value == 0x00U) {
                if (run_length == 0)
                    run_start = i;
                run_length += 8UL;
                free_count += 8UL;
                if (run_length > maximum)
                    maximum = run_length;
                if (first == (ULONG)-1 && run_length >= cell_count)
                    first = run_start;
                i += 8UL;
                continue;
            }
            if (byte_value == 0xFFU) {
                run_length = 0;
                i += 8UL;
                continue;
            }
        }
        bit_mask = (UCHAR)(1U << (i & 7UL));
        if ((bitmap[byte_index] & bit_mask) == 0) {
            if (run_length == 0)
                run_start = i;
            ++run_length;
            ++free_count;
            if (run_length > maximum)
                maximum = run_length;
            if (first == (ULONG)-1 && run_length >= cell_count)
                first = run_start;
        } else {
            run_length = 0;
        }
        ++i;
    }
    if (largest_run != (ULONG *)0)
        *largest_run = maximum;
    if (free_cells != (ULONG *)0)
        *free_cells = free_count;
    return first;
}

/* Compute the exact maximum run after a hypothetical bitmap change.  The
 * bitmap is never written here; both Alloc and Free can therefore validate
 * every structural destination before committing any mutation. */
static ULONG mempool_max_free_run_after(MEMPOOL_PAGE *page,
                                         ULONG mutation_index,
                                         ULONG mutation_count,
                                         int make_occupied)
{
    UCHAR *bitmap = (UCHAR *)0;
    ULONG i = 0;
    ULONG run_length = 0;
    ULONG maximum = 0;
    ULONG byte_index = 0;
    UCHAR bit_mask = 0;
    int occupied = 0;

    if (page == (MEMPOOL_PAGE *)0 ||
        mutation_index > MEMPOOL_NUM_PAGE_CELLS ||
        mutation_count > MEMPOOL_NUM_PAGE_CELLS - mutation_index)
        return MEMPOOL_NUM_PAGE_CELLS + 1UL;
    bitmap = (UCHAR *)page + MEMPOOL_PAGE_HEADER_SIZE;
    while (i < MEMPOOL_NUM_PAGE_CELLS) {
        byte_index = i / 8UL;
        bit_mask = (UCHAR)(1U << (i & 7UL));
        occupied = (bitmap[byte_index] & bit_mask) != 0;
        if (i >= mutation_index &&
            i - mutation_index < mutation_count)
            occupied = make_occupied != 0;
        if (occupied != 0) {
            run_length = 0;
        } else {
            ++run_length;
            if (run_length > maximum)
                maximum = run_length;
        }
        ++i;
    }
    return maximum;
}

static void *mempool_get_cells(POOL *pool, ULONG cell_count)
{
    MEMPOOL_PAGE *page = (MEMPOOL_PAGE *)0;
    MEMPOOL_PAGE *candidate = (MEMPOOL_PAGE *)0;
    UCHAR *bitmap = (UCHAR *)0;
    MEMPOOL_CELL_COUNT_ENTRY *lengths = (MEMPOOL_CELL_COUNT_ENTRY *)0;
    ULONG index = 0;
    ULONG i = 0;
    ULONG largest_run = 0;
    ULONG free_cells = 0;
    ULONG new_max_free_run = 0;
    ULONG run = 0;
    ULONG mask = 0;
    ULONG remaining = 0;
    UCHAR *address = (UCHAR *)0;
    MEMPOOL_LOCK_DECL
    MEMPOOL_LOCK_MODE_DECL

    /* Every page is in exactly one bin keyed by its exact maximum free run.
       Scanning bin heads has a fixed bound independent of the page count. */
    if (cell_count == 0 || cell_count > MEMPOOL_NUM_PAGE_CELLS ||
        cell_count > MEMPOOL_CELL_COUNT_ENTRY_MAX)
        return (void *)0;
    for (;;) {
        MEMPOOL_LOCK(pool);
        if (!mempool_page_index_counts_valid(pool)) {
            mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
            MEMPOOL_UNLOCK(pool);
            if (candidate != (MEMPOOL_PAGE *)0)
                mempool_free_mem(candidate, pool->eyecatcher);
            return (void *)0;
#endif
        }
        page = (MEMPOOL_PAGE *)0;
        run = cell_count;
        while (run < MEMPOOL_RUN_BIN_COUNT) {
            if (pool->run_bin_counts[run] != (USHORT)0) {
                page = pool->run_bins[run];
                break;
            }
            ++run;
        }
        if (page != (MEMPOOL_PAGE *)0) {
            largest_run = 0;
            if (!mempool_run_bin_page_validate(pool, page, run) ||
                page->num_free > (USHORT)MEMPOOL_NUM_PAGE_CELLS ||
                page->num_used > (USHORT)MEMPOOL_NUM_PAGE_CELLS ||
                (ULONG)page->num_free + (ULONG)page->num_used !=
                    MEMPOOL_NUM_PAGE_CELLS ||
                page->num_free < (USHORT)cell_count ||
                (ULONG)page->num_free < run) {
                mempool_abend(MEMPOOL_ABEND_GET_CELLS_HEADER);
#ifndef _KERNEL_MODE
                MEMPOOL_UNLOCK(pool);
                if (candidate != (MEMPOOL_PAGE *)0)
                    mempool_free_mem(candidate, pool->eyecatcher);
                return (void *)0;
#endif
            }
            index = mempool_find_cells(page, cell_count, &largest_run,
                                       &free_cells);
            if (index == (ULONG)-1 || largest_run != run ||
                free_cells != (ULONG)page->num_free) {
                mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
                MEMPOOL_UNLOCK(pool);
                if (candidate != (MEMPOOL_PAGE *)0)
                    mempool_free_mem(candidate, pool->eyecatcher);
                return (void *)0;
#endif
            }
            new_max_free_run = mempool_max_free_run_after(
                                   page, index, cell_count, 1);
            if (new_max_free_run > MEMPOOL_NUM_PAGE_CELLS ||
                new_max_free_run > (ULONG)page->num_free - cell_count ||
                (new_max_free_run != run &&
                 !mempool_run_bin_insert_validate(pool,
                                                   new_max_free_run))) {
                mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
                MEMPOOL_UNLOCK(pool);
                if (candidate != (MEMPOOL_PAGE *)0)
                    mempool_free_mem(candidate, pool->eyecatcher);
                return (void *)0;
#endif
            }
            lengths = (MEMPOOL_CELL_COUNT_ENTRY *)(
                          (UCHAR *)page + MEMPOOL_PAGE_HEADER_SIZE +
                          MEMPOOL_PAGE_BITMAP_SIZE);
            for (i = 0; i < cell_count; ++i) {
                if (lengths[index + i] != 0) {
                    mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
                    MEMPOOL_UNLOCK(pool);
                    if (candidate != (MEMPOOL_PAGE *)0)
                        mempool_free_mem(candidate, pool->eyecatcher);
                    return (void *)0;
#endif
                }
            }
        }
        if (page == (MEMPOOL_PAGE *)0 &&
            pool->page_count == MEMPOOL_PAGE_COUNT_LIMIT) {
            /* A compact USHORT bin count deliberately caps one pool at
               65,535 pages.  Capacity exhaustion is not corruption. */
            MEMPOOL_UNLOCK(pool);
            if (candidate != (MEMPOOL_PAGE *)0)
                mempool_free_mem(candidate, pool->eyecatcher);
            return (void *)0;
        }
        if (candidate != (MEMPOOL_PAGE *)0) {
            if (page != (MEMPOOL_PAGE *)0) {
                /* Another operation made a suitable page available while the
                   OS allocation was outside the lock.  Use it and discard the
                   still-private candidate after unlocking. */
            } else {
                largest_run = 0;
                index = mempool_find_cells(candidate, cell_count,
                                           &largest_run, &free_cells);
                new_max_free_run = mempool_max_free_run_after(
                                       candidate, index, cell_count, 1);
                if (index != 0 ||
                    largest_run != MEMPOOL_NUM_PAGE_CELLS ||
                    free_cells != MEMPOOL_NUM_PAGE_CELLS ||
                    candidate->pool != pool ||
                    candidate->eyecatcher != pool->eyecatcher ||
                    candidate->num_free !=
                        (USHORT)MEMPOOL_NUM_PAGE_CELLS ||
                    candidate->num_used != (USHORT)0 ||
                    candidate->max_free_run !=
                        (USHORT)MEMPOOL_NUM_PAGE_CELLS ||
                    new_max_free_run !=
                        MEMPOOL_NUM_PAGE_CELLS - cell_count ||
                    !mempool_page_list_insert_validate(pool) ||
                    !mempool_run_bin_insert_validate(pool,
                                                      new_max_free_run)) {
                    mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
                    MEMPOOL_UNLOCK(pool);
                    mempool_free_mem(candidate, pool->eyecatcher);
                    return (void *)0;
#endif
                }
                lengths = (MEMPOOL_CELL_COUNT_ENTRY *)(
                              (UCHAR *)candidate +
                              MEMPOOL_PAGE_HEADER_SIZE +
                              MEMPOOL_PAGE_BITMAP_SIZE);
                for (i = 0; i < cell_count; ++i) {
                    if (lengths[i] != 0) {
                        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
                        MEMPOOL_UNLOCK(pool);
                        mempool_free_mem(candidate, pool->eyecatcher);
                        return (void *)0;
#endif
                    }
                }
            /* Publish the owner/hash entry while the pool lock serializes the
               owner chain.  Destroy may have closed admission while the OS
               allocation was in progress, in which case nothing is linked. */
                if (!mempool_backing_publish(&candidate->backing)) {
                    MEMPOOL_UNLOCK(pool);
                    mempool_free_mem(candidate, pool->eyecatcher);
                    return (void *)0;
                }
                page = candidate;
                candidate = (MEMPOOL_PAGE *)0;
                mempool_page_list_insert_unchecked(pool, page);
                mempool_run_bin_insert_unchecked(pool, page,
                                                  new_max_free_run);
                run = new_max_free_run;
            }
        }
        if (page != (MEMPOOL_PAGE *)0)
            break;
        MEMPOOL_UNLOCK(pool);
        /* OS allocation can block and must never run under the pool lock. */
        candidate = mempool_alloc_page(pool, pool->pool_type,
                                       pool->eyecatcher);
        if (candidate == (MEMPOOL_PAGE *)0)
            return (void *)0;
    }
    /* All validation and every fallible publication step precede this point.
       The remaining bin, length, counter, and bitmap writes cannot fail. */
    lengths = (MEMPOOL_CELL_COUNT_ENTRY *)((UCHAR *)page +
                                           MEMPOOL_PAGE_HEADER_SIZE +
                                           MEMPOOL_PAGE_BITMAP_SIZE);
    if (new_max_free_run != run) {
        mempool_run_bin_remove_unchecked(pool, page, run);
        mempool_run_bin_insert_unchecked(pool, page, new_max_free_run);
    }
    lengths[index] = (MEMPOOL_CELL_COUNT_ENTRY)cell_count;
    page->num_free = (USHORT)(page->num_free - cell_count);
    page->num_used = (USHORT)(page->num_used + cell_count);
    page->max_free_run = (USHORT)new_max_free_run;
    bitmap = (UCHAR *)page + MEMPOOL_PAGE_HEADER_SIZE + index / 8UL;
    mask = 1UL << (index & 7UL);
    remaining = cell_count;
    while (remaining != 0) {
        /* Once aligned to a byte, mark eight cells at once. */
        if (mask == 1UL && remaining > 8UL) {
            *bitmap = 0xFFU;
            ++bitmap;
            remaining -= 8UL;
        } else {
            *bitmap |= (UCHAR)mask;
            mask <<= 1;
            mask = (mask & 0xFFUL) | (mask >> 8);
            bitmap += mask & 1UL;
            --remaining;
        }
    }
    address = (UCHAR *)page + MEMPOOL_PAGE_DATA_OFFSET +
              index * MEMPOOL_CELL_SIZE;
    MEMPOOL_UNLOCK(pool);
    if (candidate != (MEMPOOL_PAGE *)0)
        mempool_free_mem(candidate, pool->eyecatcher);
    return address;
}

static void mempool_free_cells(MEMPOOL_PAGE *page, void *address,
                               ULONG total_size)
{
    MEMPOOL_PAGE *calculated_page = (MEMPOOL_PAGE *)0;
    POOL *pool = (POOL *)0;
    UCHAR *bitmap = (UCHAR *)0;
    MEMPOOL_CELL_COUNT_ENTRY *lengths = (MEMPOOL_CELL_COUNT_ENTRY *)0;
    ULONG cell_count = 0;
    ULONG offset = 0;
    ULONG index = 0;
    ULONG i = 0;
    ULONG byte_index = 0;
    ULONG bit_mask = 0;
    ULONG mask = 0;
    ULONG remaining = 0;
    ULONG old_max_free_run = 0;
    ULONG new_max_free_run = 0;
    ULONG actual_max_free_run = 0;
    ULONG actual_free_cells = 0;
    LONG backing_references = -1;
    int reclaim_page = 0;
    MEMPOOL_LOCK_DECL
    MEMPOOL_LOCK_MODE_DECL

    if (address == (void *)0 ||
        total_size < (ULONG)MEMPOOL_ALLOCATION_HEADER_SIZE)
        return;
    cell_count = MEMPOOL_NUM_CELLS(total_size);
    /* The page and cell index are recovered from the aligned cell address;
       the separate length table then validates the exact allocation span. */
    calculated_page = (MEMPOOL_PAGE *)((ULONG_PTR)address &
                                       ~MEMPOOL_PAGE_MASK);
    if (page != calculated_page) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_RANGE);
        return;
    }
    offset = (ULONG)((ULONG_PTR)address & MEMPOOL_PAGE_MASK);
    if (offset < MEMPOOL_PAGE_DATA_OFFSET ||
        offset >= MEMPOOL_PAGE_SIZE) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_RANGE);
        return;
    }
    if ((offset - MEMPOOL_PAGE_DATA_OFFSET) %
        MEMPOOL_CELL_SIZE != 0) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_RANGE);
        return;
    }
    index = (ULONG)((offset - MEMPOOL_PAGE_DATA_OFFSET) /
                    MEMPOOL_CELL_SIZE);
    if (index >= MEMPOOL_NUM_PAGE_CELLS ||
        cell_count > MEMPOOL_NUM_PAGE_CELLS - index) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_RANGE);
        return;
    }
    pool = page->pool;
    if (pool == (POOL *)0 || page->eyecatcher != pool->eyecatcher) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_HEADER);
        return;
    }
    MEMPOOL_LOCK(pool);
    if (!mempool_page_index_counts_valid(pool) ||
        page->num_free > (USHORT)MEMPOOL_NUM_PAGE_CELLS ||
        page->num_used > (USHORT)MEMPOOL_NUM_PAGE_CELLS ||
        (ULONG)page->num_free + (ULONG)page->num_used !=
            MEMPOOL_NUM_PAGE_CELLS ||
        page->max_free_run > (USHORT)MEMPOOL_NUM_PAGE_CELLS ||
        (ULONG)page->max_free_run > (ULONG)page->num_free ||
        page->num_used < (USHORT)cell_count ||
        (ULONG)page->num_free > MEMPOOL_NUM_PAGE_CELLS - cell_count) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_COUNT);
#ifndef _KERNEL_MODE
        MEMPOOL_UNLOCK(pool);
        return;
#endif
    }
    old_max_free_run = (ULONG)page->max_free_run;
    if (!mempool_run_bin_page_validate(pool, page, old_max_free_run)) {
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        MEMPOOL_UNLOCK(pool);
        return;
#endif
    }
    bitmap = (UCHAR *)page + MEMPOOL_PAGE_HEADER_SIZE;
    lengths = (MEMPOOL_CELL_COUNT_ENTRY *)((UCHAR *)page +
                                           MEMPOOL_PAGE_HEADER_SIZE +
                                           MEMPOOL_PAGE_BITMAP_SIZE);
    /* This independent table binds the caller-visible start to its exact
       rounded extent.  A damaged hidden size can never consume a neighbour. */
    if (lengths[index] != (MEMPOOL_CELL_COUNT_ENTRY)cell_count) {
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        MEMPOOL_UNLOCK(pool);
        return;
#endif
    }
    for (i = 1; i < cell_count; ++i) {
        if (lengths[index + i] != 0) {
            mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
            MEMPOOL_UNLOCK(pool);
            return;
#endif
        }
    }
    /* Check before clearing so a double free is reported rather than silently
       inflating either accounting counter. */
    for (i = 0; i < cell_count; ++i) {
        byte_index = (index + i) / 8UL;
        bit_mask = 1UL << ((index + i) & 7UL);
        if ((bitmap[byte_index] & (UCHAR)bit_mask) == 0) {
            mempool_abend(MEMPOOL_ABEND_FREE_CELLS_DOUBLE);
#ifndef _KERNEL_MODE
            MEMPOOL_UNLOCK(pool);
            return;
#endif
        }
    }
    /* Reconcile the exact counters/bin with the whole bounded bitmap before
       using num_used to decide that this is the last allocation on a page. */
    (void)mempool_find_cells(page, MEMPOOL_NUM_PAGE_CELLS + 1UL,
                             &actual_max_free_run, &actual_free_cells);
    if (actual_max_free_run != old_max_free_run ||
        actual_free_cells != (ULONG)page->num_free) {
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        MEMPOOL_UNLOCK(pool);
        return;
#endif
    }
    new_max_free_run = mempool_max_free_run_after(page, index, cell_count, 0);
    if (new_max_free_run > MEMPOOL_NUM_PAGE_CELLS ||
        new_max_free_run < old_max_free_run ||
        new_max_free_run > (ULONG)page->num_free + cell_count) {
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        MEMPOOL_UNLOCK(pool);
        return;
#endif
    }
    /* A page that becomes empty is retired.  The first page cannot be
       reclaimed because it contains POOL and its synchronization object. */
    reclaim_page = page !=
                   (MEMPOOL_PAGE *)((ULONG_PTR)pool & ~MEMPOOL_PAGE_MASK) &&
                   page->num_used == (USHORT)cell_count;
    if ((reclaim_page != 0 &&
         !mempool_page_list_remove_validate(pool, page)) ||
        (reclaim_page == 0 && new_max_free_run != old_max_free_run &&
         !mempool_run_bin_insert_validate(pool, new_max_free_run))) {
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        MEMPOOL_UNLOCK(pool);
        return;
#endif
    }
    if (reclaim_page != 0) {
        /* Retirement is the final fallible step.  Bin/list neighbors were
           identity-validated above, so everything after it is a no-fail
           unlink/accounting commit. */
        backing_references = mempool_backing_retire(pool->registration,
                                                    &page->backing);
        if (backing_references < 1) {
            mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
            MEMPOOL_UNLOCK(pool);
            return;
#endif
        }
    }
    if (reclaim_page != 0) {
        mempool_run_bin_remove_unchecked(pool, page, old_max_free_run);
        mempool_page_list_remove_unchecked(pool, page);
    } else if (new_max_free_run != old_max_free_run) {
        mempool_run_bin_remove_unchecked(pool, page, old_max_free_run);
        mempool_run_bin_insert_unchecked(pool, page, new_max_free_run);
    }
    page->num_free = (USHORT)(page->num_free + cell_count);
    page->num_used = (USHORT)(page->num_used - cell_count);
    page->max_free_run = (USHORT)new_max_free_run;
    lengths[index] = 0;
    bitmap += index / 8UL;
    mask = 1UL << (index & 7UL);
    remaining = cell_count;
    while (remaining != 0) {
        /* Mirror allocation: clear complete free bytes in one store. */
        if (mask == 1UL && remaining > 8UL) {
            *bitmap = 0x00U;
            ++bitmap;
            remaining -= 8UL;
        } else {
            *bitmap &= (UCHAR)~mask;
            mask <<= 1;
            mask = (mask & 0xFFUL) | (mask >> 8);
            bitmap += mask & 1UL;
            --remaining;
        }
    }
    MEMPOOL_UNLOCK(pool);
}

/* ------------------------------------------------------------------------- */
/* Large chunk allocator                                                     */
/* ------------------------------------------------------------------------- */

static int mempool_large_chunk_count_valid(POOL *pool)
{
    if (pool == (POOL *)0 ||
        pool->large_chunk_count + pool->large_chunk_count_check != 0 ||
        pool->large_chunk_count > MEMPOOL_REGISTRY_CAPACITY - 1UL ||
        pool->large_chunks.count < 0 ||
        (ULONG)pool->large_chunks.count != pool->large_chunk_count)
        return 0;
    if (pool->large_chunk_count == 0)
        return pool->large_chunks.head == (LIST_ELEM *)0 &&
               pool->large_chunks.tail == (LIST_ELEM *)0;
    return pool->large_chunks.head != (LIST_ELEM *)0 &&
           pool->large_chunks.tail != (LIST_ELEM *)0;
}

/* The registry lock is held.  Identity lookup makes the intrusive-list
 * address safe before any chunk field is read; base lookup then proves that
 * both independent backing indexes still agree. */
static int mempool_large_chunk_node_valid_locked(
    POOL *pool,
    MEMPOOL_LARGE_CHUNK *chunk)
{
    MEMPOOL_BACKING *backing = (MEMPOOL_BACKING *)0;
    ULONG_PTR chunk_value = (ULONG_PTR)0;

    if (pool == (POOL *)0 || chunk == (MEMPOOL_LARGE_CHUNK *)0)
        return 0;
    chunk_value = (ULONG_PTR)chunk;
    if (chunk_value > ~(ULONG_PTR)0 -
                      (ULONG_PTR)offsetof(MEMPOOL_LARGE_CHUNK, backing))
        return 0;
    backing = (MEMPOOL_BACKING *)(chunk_value +
              (ULONG_PTR)offsetof(MEMPOOL_LARGE_CHUNK, backing));
    if (mempool_backing_identity_find_locked(backing) != backing)
        return 0;
    return mempool_backing_find_locked(backing->base) == backing &&
           backing->object == chunk &&
           backing->registration == pool->registration &&
           backing->kind == MEMPOOL_BACKING_LARGE &&
           backing->base == chunk->ptr && backing->size == chunk->size &&
           backing->tag == pool->eyecatcher &&
           InterlockedCompareExchange(&backing->closing, 0, 0) == 0 &&
           InterlockedCompareExchange(&backing->references, 0, 0) > 0 &&
           chunk->pool == pool && chunk->eyecatcher == pool->eyecatcher;
}

static int mempool_large_chunk_list_insert_validate(POOL *pool)
{
    MEMPOOL_LARGE_CHUNK *head = (MEMPOOL_LARGE_CHUNK *)0;
    MEMPOOL_LARGE_CHUNK *tail = (MEMPOOL_LARGE_CHUNK *)0;
    MEMPOOL_LARGE_CHUNK *head_next = (MEMPOOL_LARGE_CHUNK *)0;
    MEMPOOL_LARGE_CHUNK *tail_previous = (MEMPOOL_LARGE_CHUNK *)0;
    int valid = 1;
    MEMPOOL_REGISTRY_LOCK_DECL

    if (!mempool_large_chunk_count_valid(pool) ||
        pool->large_chunk_count >= MEMPOOL_REGISTRY_CAPACITY - 1UL)
        return 0;
    if (pool->large_chunk_count == 0)
        return 1;
    head = (MEMPOOL_LARGE_CHUNK *)pool->large_chunks.head;
    tail = (MEMPOOL_LARGE_CHUNK *)pool->large_chunks.tail;

    MEMPOOL_REGISTRY_LOCK();
    if (!mempool_large_chunk_node_valid_locked(pool, head) ||
        !mempool_large_chunk_node_valid_locked(pool, tail) ||
        head->list_elem.prev != (LIST_ELEM *)0 ||
        tail->list_elem.next != (LIST_ELEM *)0 ||
        ((pool->large_chunk_count == 1) != (head == tail))) {
        valid = 0;
    } else if (pool->large_chunk_count == 1) {
        if (head->list_elem.next != (LIST_ELEM *)0)
            valid = 0;
    } else {
        head_next = (MEMPOOL_LARGE_CHUNK *)head->list_elem.next;
        tail_previous = (MEMPOOL_LARGE_CHUNK *)tail->list_elem.prev;
        if (head_next == (MEMPOOL_LARGE_CHUNK *)0 ||
            tail_previous == (MEMPOOL_LARGE_CHUNK *)0 ||
            head_next == head || tail_previous == tail ||
            !mempool_large_chunk_node_valid_locked(pool, head_next) ||
            !mempool_large_chunk_node_valid_locked(pool, tail_previous) ||
            head_next->list_elem.prev != (LIST_ELEM *)head ||
            tail_previous->list_elem.next != (LIST_ELEM *)tail)
            valid = 0;
    }
    MEMPOOL_REGISTRY_UNLOCK();
    return valid;
}

static void mempool_large_chunk_list_insert_unchecked(
    POOL *pool,
    MEMPOOL_LARGE_CHUNK *chunk)
{
    LIST_ELEM *head = pool->large_chunks.head;

    chunk->list_elem.prev = (LIST_ELEM *)0;
    chunk->list_elem.next = head;
    if (head != (LIST_ELEM *)0)
        head->prev = (LIST_ELEM *)chunk;
    else
        pool->large_chunks.tail = (LIST_ELEM *)chunk;
    pool->large_chunks.head = (LIST_ELEM *)chunk;
    ++pool->large_chunks.count;
    ++pool->large_chunk_count;
    --pool->large_chunk_count_check;
}

static void mempool_large_chunk_list_remove_unchecked(
    POOL *pool,
    MEMPOOL_LARGE_CHUNK *chunk)
{
    LIST_ELEM *previous = chunk->list_elem.prev;
    LIST_ELEM *next = chunk->list_elem.next;

    if (previous != (LIST_ELEM *)0)
        previous->next = next;
    else
        pool->large_chunks.head = next;
    if (next != (LIST_ELEM *)0)
        next->prev = previous;
    else
        pool->large_chunks.tail = previous;
    chunk->list_elem.next = (LIST_ELEM *)0;
    chunk->list_elem.prev = (LIST_ELEM *)0;
    --pool->large_chunks.count;
    --pool->large_chunk_count;
    ++pool->large_chunk_count_check;
}

static void *mempool_get_large_chunk(POOL *pool, ULONG size)
{
    ULONG large_size = 0;
    void *address = (void *)0;
    MEMPOOL_LARGE_CHUNK *chunk = (MEMPOOL_LARGE_CHUNK *)0;
    MEMPOOL_LOCK_DECL
    MEMPOOL_LOCK_MODE_DECL

    /* The descriptor is separately allocated resident metadata; the complete
       rounded backing therefore belongs to the allocation payload/header. */
    if (size > MEMPOOL_LARGE_CHUNK_MAXIMUM)
        return (void *)0;
    large_size = (size + MEMPOOL_PAGE_SIZE - 1UL) &
                 ~(MEMPOOL_PAGE_SIZE - 1UL);
    address = mempool_alloc_mem(pool->pool_type, large_size, pool->eyecatcher);
    if (address == (void *)0)
        return (void *)0;
    chunk = (MEMPOOL_LARGE_CHUNK *)mempool_metadata_alloc(
                (SIZE_T)sizeof(MEMPOOL_LARGE_CHUNK));
    if (chunk == (MEMPOOL_LARGE_CHUNK *)0) {
        mempool_free_mem(address, pool->eyecatcher);
        return (void *)0;
    }
    chunk->list_elem.next = (LIST_ELEM *)0;
    chunk->list_elem.prev = (LIST_ELEM *)0;
    chunk->eyecatcher = pool->eyecatcher;
    chunk->pool = pool;
    chunk->ptr = address;
    chunk->size = large_size;
    chunk->allocation_size = size;
    chunk->cookie = mempool_allocation_cookie(pool, address, size);
    mempool_backing_prepare(&chunk->backing, pool->registration, address,
                            chunk, large_size, pool->eyecatcher,
                            MEMPOOL_BACKING_LARGE);
    /* Serialize owner publication with other changes to this pool's chain. */
    MEMPOOL_LOCK(pool);
    if (!mempool_large_chunk_count_valid(pool)) {
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        MEMPOOL_UNLOCK(pool);
        mempool_free_mem(address, pool->eyecatcher);
        mempool_metadata_free(chunk);
        return (void *)0;
#endif
    }
    if (pool->large_chunk_count >= MEMPOOL_REGISTRY_CAPACITY - 1UL) {
        MEMPOOL_UNLOCK(pool);
        mempool_free_mem(address, pool->eyecatcher);
        mempool_metadata_free(chunk);
        return (void *)0;
    }
    if (!mempool_large_chunk_list_insert_validate(pool)) {
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        MEMPOOL_UNLOCK(pool);
        mempool_free_mem(address, pool->eyecatcher);
        mempool_metadata_free(chunk);
        return (void *)0;
#endif
    }
    if (!mempool_backing_publish(&chunk->backing)) {
        MEMPOOL_UNLOCK(pool);
        mempool_free_mem(address, pool->eyecatcher);
        mempool_metadata_free(chunk);
        return (void *)0;
    }
    mempool_large_chunk_list_insert_unchecked(pool, chunk);
    MEMPOOL_UNLOCK(pool);
    return address;
}

/* The backing registry has already pinned chunk.  Validate only its local
 * intrusive-list neighborhood so large frees remain independent of the total
 * number of outstanding large allocations at DISPATCH_LEVEL. */
static int mempool_large_chunk_list_validate(POOL *pool,
                                              MEMPOOL_LARGE_CHUNK *chunk)
{
    LIST_ELEM *element = (LIST_ELEM *)0;
    LIST_ELEM *previous = (LIST_ELEM *)0;
    LIST_ELEM *next = (LIST_ELEM *)0;
    MEMPOOL_LARGE_CHUNK *head = (MEMPOOL_LARGE_CHUNK *)0;
    MEMPOOL_LARGE_CHUNK *tail = (MEMPOOL_LARGE_CHUNK *)0;
    MEMPOOL_LARGE_CHUNK *previous_chunk = (MEMPOOL_LARGE_CHUNK *)0;
    MEMPOOL_LARGE_CHUNK *next_chunk = (MEMPOOL_LARGE_CHUNK *)0;
    int valid = 1;
    MEMPOOL_REGISTRY_LOCK_DECL

    if (pool == (POOL *)0 || chunk == (MEMPOOL_LARGE_CHUNK *)0 ||
        !mempool_large_chunk_count_valid(pool) ||
        pool->large_chunk_count == 0)
        return 0;
    element = (LIST_ELEM *)chunk;
    head = (MEMPOOL_LARGE_CHUNK *)pool->large_chunks.head;
    tail = (MEMPOOL_LARGE_CHUNK *)pool->large_chunks.tail;

    MEMPOOL_REGISTRY_LOCK();
    if (!mempool_large_chunk_node_valid_locked(pool, chunk)) {
        valid = 0;
    } else {
        previous = element->prev;
        next = element->next;
        previous_chunk = (MEMPOOL_LARGE_CHUNK *)previous;
        next_chunk = (MEMPOOL_LARGE_CHUNK *)next;
        if (previous == element || next == element ||
            (previous != (LIST_ELEM *)0 && previous == next) ||
            ((pool->large_chunks.head == element) !=
             (previous == (LIST_ELEM *)0)) ||
            ((pool->large_chunks.tail == element) !=
             (next == (LIST_ELEM *)0)) ||
            ((pool->large_chunk_count == 1) !=
             (previous == (LIST_ELEM *)0 && next == (LIST_ELEM *)0)) ||
            !mempool_large_chunk_node_valid_locked(pool, head) ||
            !mempool_large_chunk_node_valid_locked(pool, tail) ||
            head->list_elem.prev != (LIST_ELEM *)0 ||
            tail->list_elem.next != (LIST_ELEM *)0 ||
            ((pool->large_chunk_count == 1) != (head == tail))) {
            valid = 0;
        } else if (previous_chunk != (MEMPOOL_LARGE_CHUNK *)0 &&
                   (!mempool_large_chunk_node_valid_locked(pool,
                                                            previous_chunk) ||
                    previous_chunk->list_elem.next != element)) {
            valid = 0;
        } else if (next_chunk != (MEMPOOL_LARGE_CHUNK *)0 &&
                   (!mempool_large_chunk_node_valid_locked(pool,
                                                            next_chunk) ||
                    next_chunk->list_elem.prev != element)) {
            valid = 0;
        }
    }
    MEMPOOL_REGISTRY_UNLOCK();
    return valid;
}

static void mempool_free_large_chunk(MEMPOOL_LARGE_CHUNK *chunk,
                                     void *address, ULONG size)
{
    POOL *pool = (POOL *)0;
    LONG backing_references = -1;
    MEMPOOL_LOCK_DECL
    MEMPOOL_LOCK_MODE_DECL

    pool = chunk->pool;
    if (pool == (POOL *)0) {
        mempool_abend(MEMPOOL_ABEND_LARGE_HEADER);
        return;
    }
    MEMPOOL_LOCK(pool);
    if (chunk->pool != pool || chunk->eyecatcher != pool->eyecatcher ||
        chunk->backing.base != address || chunk->backing.object != chunk ||
        chunk->backing.registration != pool->registration ||
        chunk->backing.kind != MEMPOOL_BACKING_LARGE ||
        chunk->backing.size != chunk->size ||
        chunk->allocation_size != size ||
        chunk->cookie != mempool_allocation_cookie(pool, address, size) ||
        !mempool_large_chunk_list_validate(pool, chunk)) {
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        MEMPOOL_UNLOCK(pool);
        return;
#endif
    }
    backing_references = mempool_backing_retire(pool->registration,
                                                &chunk->backing);
    if (backing_references < 1) {
        mempool_abend(MEMPOOL_ABEND_ALLOCATION_METADATA);
#ifndef _KERNEL_MODE
        MEMPOOL_UNLOCK(pool);
        return;
#endif
    }
    /* All subsequent list writes are infallible and locally prevalidated. */
    mempool_large_chunk_list_remove_unchecked(pool, chunk);
    MEMPOOL_UNLOCK(pool);
}
