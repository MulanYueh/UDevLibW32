/*
 * Windows user-mode/kernel-mode memory pool.
 *
 * Small allocations are served from page-local fixed-size cells tracked by a
 * bitmap.  Allocations near a page or larger are page-aligned large chunks;
 * their descriptor is stored at the end of the allocation.  A pool owns all
 * pages and large chunks and releases them from Mempool_DestroyPool.
 *
 * Coding rule: every function-local object is initialized at its declaration.
 * Keep declarations at the beginning of each function/block for old MSVC.
 */

#include "mempool.h"
#include "def.h"
#include "allocator.h"

#ifndef _KERNEL_MODE
/*
 * The project CRT is freestanding and is also available to user-mode
 * callers.  Keep this source's dependency narrow instead of including the
 * full libc.h: that public header also declares optional C99-era utilities
 * which old C compilers do not need to parse.  These declarations mirror the
 * raw-memory ABI in libc.h; link libc.c when building the user-mode allocator.
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
#endif

/*
 * Design overview
 * ----------------
 *
 * The allocator has two paths.  Small requests are rounded to fixed-size
 * cells and served from a page bitmap.  Large requests are rounded to whole
 * pages and tracked as independent chunks.  This avoids asking the OS for
 * every small object while keeping large objects out of the bitmap search.
 *
 * A normal page looks like this (one bitmap bit describes one cell):
 *
 *   page base
 *       | page header | bitmap | cell 0 | cell 1 | ... | cell N-1 |
 *       +-------------+--------+--------+--------+-----+----------+
 *                              ^
 *                              internal cell start (size word at +0,
 *                              caller pointer after the hidden aligned header)
 *
 * The first page is special: the POOL object is placed where cell 0 starts.
 * Its cells are marked used in the bitmap, so they can never be returned to
 * a caller:
 *
 *   page base
 *       | header | bitmap | POOL + initial_bitmap | free cells ... |
 *       +--------+--------+-----------------------+----------------+
 *                             ^
 *                             pool pointer
 *
 * Every allocation stores its total internal size (requested bytes plus the
 * hidden aligned header) at the beginning of that header and the owning pool
 * pointer in the second pointer-sized slot.  Mempool_Free uses that owner slot
 * to enter the pool's lifetime gate before touching page metadata; this closes
 * the otherwise racy window between a concurrent Free and page destruction.
 *
 * Page ownership is represented by two intrusive lists:
 *
 *     pool->pages      -> [page] -> [page] -> NULL   (searched)
 *     pool->full_pages -> [page] -> NULL            (skipped until freed)
 *     pool->large_chunks -> [chunk] -> [chunk] -> NULL
 */

typedef struct MEMPOOL_LIST_ELEM MEMPOOL_LIST_ELEM;
typedef struct MEMPOOL_LIST MEMPOOL_LIST;

/*
 * Lists are intrusive: the first bytes of PAGE and LARGE_CHUNK are the list
 * node itself.  No list node is allocated separately, which keeps page
 * insertion/removal constant-time and avoids another allocator dependency.
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
#define List_Head(list) ((void *)((list)->head))
#define List_Next(element) ((void *)(((LIST_ELEM *)(element))->next))

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

#ifndef MEMPOOL_FULL_PAGE_THRESHOLD
/* Pages below this exact free-cell count are skipped during searches. */
#define MEMPOOL_FULL_PAGE_THRESHOLD 4UL
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
#define MEMPOOL_PAD_8(value) (((value) + 7UL) & ~7UL)
#define MEMPOOL_ALLOCATION_HEADER_SIZE \
    ((SIZE_T)(sizeof(ULONG_PTR) * 2UL))
#define MEMPOOL_NUM_CELLS(value) \
    (MEMPOOL_PAD_CELL(value) / MEMPOOL_CELL_SIZE)
#define MEMPOOL_PAGE_MASK ((ULONG_PTR)MEMPOOL_PAGE_SIZE - (ULONG_PTR)1)

typedef struct MEMPOOL_PAGE MEMPOOL_PAGE;
typedef struct MEMPOOL_LARGE_CHUNK MEMPOOL_LARGE_CHUNK;

struct MEMPOOL_PAGE {
    LIST_ELEM list_elem; /* Intrusive node for pages/full_pages. */
    MEMPOOL_PAGE *next;  /* Reserved for compatibility; list_elem is used. */
    POOL *pool;          /* Owning pool, recovered by Mempool_Free. */
    ULONG eyecatcher;    /* Pool tag used for corruption checks and freeing. */
    USHORT num_free;     /* Exact free-cell count used as a search filter. */
    USHORT num_used;     /* Exact number of allocated cells for reclamation. */
    USHORT max_free_run; /* Exact run cache; zero means bitmap changed/unknown. */
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
#define MEMPOOL_NUM_PAGE_CELLS \
    ((MEMPOOL_PAGE_SIZE - MEMPOOL_PAGE_HEADER_SIZE - \
      MEMPOOL_PAGE_BITMAP_SIZE) / MEMPOOL_CELL_SIZE)

struct POOL {
    ULONG eyecatcher; /* Same tag as all pages owned by this pool. */
    ULONG pool_type;  /* MEMPOOL_PAGED or MEMPOOL_NONPAGED. */
#ifdef _KERNEL_MODE
    EX_PUSH_LOCK lock; /* Non-recursive; callers must not re-enter the pool. */
#else
    CRITICAL_SECTION lock; /* Critical sections are recursive in user mode. */
#endif
    volatile LONG lifecycle_state; /* high bit=closing, low bits=active ops */
    LIST pages;       /* Pages that may satisfy a new cell request. */
    LIST full_pages;  /* Pages with fewer than FULL_PAGE_THRESHOLD cells. */
    LIST large_chunks; /* Large-chunk descriptors stored in their allocations. */
    UCHAR initial_bitmap[MEMPOOL_PAGE_BITMAP_SIZE]; /* Padding mask for new pages. */
};

struct MEMPOOL_LARGE_CHUNK {
    LIST_ELEM list_elem; /* Intrusive node for pool->large_chunks. */
    ULONG eyecatcher;    /* Tag copied from the owning pool. */
    POOL *pool;          /* Owner used to find the correct list and lock. */
    void *ptr;           /* Page-aligned OS allocation base. */
    ULONG size;          /* Entire OS allocation, including padding. */
};

#define MEMPOOL_LARGE_CHUNK_SIZE MEMPOOL_PAD_8(sizeof(MEMPOOL_LARGE_CHUNK))

/*
 * The page-rounding expression adds both the descriptor and almost one full
 * page before masking.  Keep enough headroom that this addition cannot wrap
 * a 32-bit ULONG.  This is intentionally a little conservative, matching
 * the original allocator's safe upper bound.
 */
#define MEMPOOL_LARGE_CHUNK_MAXIMUM \
    ((ULONG)0xFFFFFFFFUL - (MEMPOOL_LARGE_CHUNK_SIZE + MEMPOOL_PAGE_SIZE))
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
    MEMPOOL_ABEND_FREE_CELLS_COUNT = 8    /* Exact used-cell counter is corrupt. */
};

/* The high bit closes admission; the remaining bits count active operations. */
#define MEMPOOL_LIFECYCLE_CLOSING ((LONG)0x80000000L)
#define MEMPOOL_LIFECYCLE_COUNT_MASK ((LONG)0x7FFFFFFFL)

/* This gate protects the owner lookup in Mempool_Free from DestroyPool. */
static volatile LONG mempool_lifecycle_gate = 0;
typedef struct MEMPOOL_REGISTRATION MEMPOOL_REGISTRATION;
struct MEMPOOL_REGISTRATION {
    POOL *pool;
    MEMPOOL_REGISTRATION *next;
};
static MEMPOOL_REGISTRATION *mempool_registry =
    (MEMPOOL_REGISTRATION *)0;

static void mempool_zero(void *address, SIZE_T length);
static void mempool_copy(void *destination, const void *source, SIZE_T length);
static void *mempool_alloc_mem(ULONG pool_type, ULONG size, ULONG tag);
static void mempool_free_mem(void *address, ULONG tag);
static MEMPOOL_PAGE *mempool_alloc_page(POOL *pool, ULONG pool_type, ULONG tag);
static ULONG mempool_find_cells(MEMPOOL_PAGE *page, ULONG cell_count,
                                ULONG *largest_run);
static void *mempool_get_cells(POOL *pool, ULONG cell_count);
static void mempool_free_cells(void *address, ULONG cell_count);
static void *mempool_get_large_chunk(POOL *pool, ULONG size);
static void mempool_free_large_chunk(void *address, ULONG size);
static POOL *mempool_create(ULONG pool_type, ULONG tag);
static void mempool_abend(ULONG reason);
static void mempool_lifecycle_gate_shared_enter(void);
static void mempool_lifecycle_gate_shared_leave(void);
static void mempool_lifecycle_gate_exclusive_enter(void);
static void mempool_lifecycle_gate_exclusive_leave(void);
static int mempool_operation_enter(POOL *pool);
static void mempool_operation_leave(POOL *pool);
static int mempool_begin_destroy(POOL *pool);

static void mempool_zero(void *address, SIZE_T length)
{
#ifdef _KERNEL_MODE
    RtlZeroMemory(address, length);
#else
    /* libc_memset is the project's CRT-independent implementation; callers
       pass storage owned by the pool and a length checked by the layout. */
    libc_memset(address, 0, (size_t)length);
#endif
}

static void mempool_copy(void *destination, const void *source, SIZE_T length)
{
    /* Keep platform memory primitives in the kernel build. */
#ifdef _KERNEL_MODE
    RtlCopyMemory(destination, source, length);
#else
    /* The source and destination are non-overlapping bitmap regions.  Keep
       this primitive separate from the checked Move path, which uses memmove
       when caller ranges may overlap. */
    libc_memcpy(destination, source, (size_t)length);
#endif
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
/* List implementation                                                       */
/* ------------------------------------------------------------------------- */

/* Insert at the head when old_element is NULL; otherwise insert before it. */
static void List_Insert_Before(LIST *list, void *old_element, void *new_element)
{
    LIST_ELEM *current = (LIST_ELEM *)0;
    LIST_ELEM *new_item = (LIST_ELEM *)0;
    LIST_ELEM *previous = (LIST_ELEM *)0;

    current = (LIST_ELEM *)old_element;
    new_item = (LIST_ELEM *)new_element;
    ++list->count;
    if (current == (LIST_ELEM *)0 || current == list->head) {
        new_item->prev = (LIST_ELEM *)0;
        new_item->next = list->head;
        if (list->head != (LIST_ELEM *)0)
            list->head->prev = new_item;
        else
            list->tail = new_item;
        list->head = new_item;
    } else {
        previous = current->prev;
        previous->next = new_item;
        new_item->prev = previous;
        new_item->next = current;
        current->prev = new_item;
    }
}

/* Remove an element without freeing it; the owner frees the containing page. */
static void List_Remove(LIST *list, void *element)
{
    LIST_ELEM *current = (LIST_ELEM *)0;
    LIST_ELEM *previous = (LIST_ELEM *)0;
    LIST_ELEM *next = (LIST_ELEM *)0;

    current = (LIST_ELEM *)element;
    if (current == (LIST_ELEM *)0 || list->count <= 0)
        return;
    previous = current->prev;
    next = current->next;
    if (previous != (LIST_ELEM *)0)
        previous->next = next;
    else
        list->head = next;
    if (next != (LIST_ELEM *)0)
        next->prev = previous;
    else
        list->tail = previous;
    current->next = (LIST_ELEM *)0;
    current->prev = (LIST_ELEM *)0;
    --list->count;
}

/* ------------------------------------------------------------------------- */
/* Platform lock selection                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Only one exclusive lock is needed: it protects both page lists and the
 * bitmap state.  Large-chunk list updates use the same lock.  Destruction
 * closes admission and drains already-entered operations before it frees the
 * first page; it therefore never tries to acquire a lock from a freed pool.
 * EX_PUSH_LOCK is deliberately used without recursion in kernel mode.
 */
#ifdef _KERNEL_MODE
#define MEMPOOL_LOCK_DECL
#define MEMPOOL_LOCK(pool) \
    do { \
        KeEnterCriticalRegion(); \
        ExAcquirePushLockExclusive(&(pool)->lock); \
    } while (0)
#define MEMPOOL_UNLOCK(pool) \
    do { \
        ExReleasePushLockExclusive(&(pool)->lock); \
        KeLeaveCriticalRegion(); \
    } while (0)
#else
#define MEMPOOL_LOCK_DECL
#define MEMPOOL_LOCK(pool) EnterCriticalSection(&(pool)->lock)
#define MEMPOOL_UNLOCK(pool) LeaveCriticalSection(&(pool)->lock)
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
 * Mempool_Free has no pool argument, so a short process/driver-wide reader
 * gate protects the hidden owner-pointer lookup.  It is held only while the
 * per-pool active count is acquired, not during bitmap work or OS allocation.
 * Destroy takes the writer side before closing the selected pool.
 *
 *   Free/Alloc: global reader -> pool active++ -> global reader release
 *   Destroy:    global writer -> close pool -> wait active==0 -> release
 */
#ifdef _KERNEL_MODE
#define MEMPOOL_LIFECYCLE_YIELD() KeYieldProcessor()
#else
#define MEMPOOL_LIFECYCLE_YIELD() Sleep(0)
#endif

static void mempool_lifecycle_gate_shared_enter(void)
{
    LONG state = 0;

    for (;;) {
        state = InterlockedCompareExchange(&mempool_lifecycle_gate, 0, 0);
        if ((state & MEMPOOL_LIFECYCLE_CLOSING) != 0 ||
            (state & MEMPOOL_LIFECYCLE_COUNT_MASK) ==
                MEMPOOL_LIFECYCLE_COUNT_MASK) {
            MEMPOOL_LIFECYCLE_YIELD();
        } else if (InterlockedCompareExchange(
                       &mempool_lifecycle_gate, state + 1, state) == state) {
            return;
        }
    }
}

static void mempool_lifecycle_gate_shared_leave(void)
{
    (void)InterlockedDecrement(&mempool_lifecycle_gate);
}

static void mempool_lifecycle_gate_exclusive_enter(void)
{
    LONG state = 0;
    LONG desired = 0;

    for (;;) {
        state = InterlockedCompareExchange(&mempool_lifecycle_gate, 0, 0);
        if ((state & MEMPOOL_LIFECYCLE_CLOSING) != 0) {
            MEMPOOL_LIFECYCLE_YIELD();
            continue;
        }
        desired = state | MEMPOOL_LIFECYCLE_CLOSING;
        if (InterlockedCompareExchange(&mempool_lifecycle_gate,
                                       desired, state) == state)
            break;
    }
    while ((InterlockedCompareExchange(&mempool_lifecycle_gate, 0, 0) &
            MEMPOOL_LIFECYCLE_COUNT_MASK) != 0) {
        MEMPOOL_LIFECYCLE_YIELD();
    }
}

static void mempool_lifecycle_gate_exclusive_leave(void)
{
    (void)InterlockedExchange(&mempool_lifecycle_gate, 0);
}

static int mempool_operation_enter(POOL *pool)
{
    LONG state = 0;

    if (pool == (POOL *)0)
        return 0;
    /* The tag check rejects an obviously foreign object before taking its
       embedded lock.  It is not a substitute for the caller's lifetime rule:
       a pointer used after Destroy has returned is already invalid. */
    if (pool->eyecatcher != MEMPOOL_POOL_TAG)
        return 0;
    for (;;) {
        state = InterlockedCompareExchange(&pool->lifecycle_state, 0, 0);
        if ((state & MEMPOOL_LIFECYCLE_CLOSING) != 0 ||
            (state & MEMPOOL_LIFECYCLE_COUNT_MASK) ==
                MEMPOOL_LIFECYCLE_COUNT_MASK)
            return 0;
        if (InterlockedCompareExchange(&pool->lifecycle_state,
                                       state + 1, state) == state)
            return 1;
    }
}

static MEMPOOL_REGISTRATION *mempool_registry_find_locked(POOL *pool)
{
    MEMPOOL_REGISTRATION *registration = (MEMPOOL_REGISTRATION *)0;

    registration = mempool_registry;
    while (registration != (MEMPOOL_REGISTRATION *)0) {
        if (registration->pool == pool)
            return registration;
        registration = registration->next;
    }
    return (MEMPOOL_REGISTRATION *)0;
}

static void mempool_registry_remove_locked(
    MEMPOOL_REGISTRATION *registration)
{
    MEMPOOL_REGISTRATION *current = (MEMPOOL_REGISTRATION *)0;
    MEMPOOL_REGISTRATION *previous = (MEMPOOL_REGISTRATION *)0;

    current = mempool_registry;
    previous = (MEMPOOL_REGISTRATION *)0;
    while (current != (MEMPOOL_REGISTRATION *)0) {
        if (current == registration) {
            if (previous == (MEMPOOL_REGISTRATION *)0)
                mempool_registry = current->next;
            else
                previous->next = current->next;
            current->next = (MEMPOOL_REGISTRATION *)0;
            return;
        }
        previous = current;
        current = current->next;
    }
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

static void mempool_registry_add(MEMPOOL_REGISTRATION *registration)
{
    mempool_lifecycle_gate_exclusive_enter();
    registration->next = mempool_registry;
    mempool_registry = registration;
    mempool_lifecycle_gate_exclusive_leave();
}

static int mempool_operation_admit(POOL *pool)
{
    MEMPOOL_REGISTRATION *registration = (MEMPOOL_REGISTRATION *)0;
    int accepted = 0;

    if (pool == (POOL *)0)
        return 0;
    mempool_lifecycle_gate_shared_enter();
    registration = mempool_registry_find_locked(pool);
    if (registration != (MEMPOOL_REGISTRATION *)0)
        accepted = mempool_operation_enter(pool);
    mempool_lifecycle_gate_shared_leave();
    return accepted;
}

static void mempool_operation_leave(POOL *pool)
{
    (void)InterlockedDecrement(&pool->lifecycle_state);
}

static int mempool_begin_destroy(POOL *pool)
{
    LONG state = 0;
    LONG desired = 0;

    if (pool == (POOL *)0)
        return 0;
    if (pool->eyecatcher != MEMPOOL_POOL_TAG)
        return 0;
    for (;;) {
        state = InterlockedCompareExchange(&pool->lifecycle_state, 0, 0);
        if ((state & MEMPOOL_LIFECYCLE_CLOSING) != 0)
            return 0;
        desired = state | MEMPOOL_LIFECYCLE_CLOSING;
        if (InterlockedCompareExchange(&pool->lifecycle_state,
                                       desired, state) == state)
            break;
    }
    while ((InterlockedCompareExchange(&pool->lifecycle_state, 0, 0) &
            MEMPOOL_LIFECYCLE_COUNT_MASK) != 0) {
        MEMPOOL_LIFECYCLE_YIELD();
    }
    return 1;
}

#undef MEMPOOL_LIFECYCLE_YIELD

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
    /* PagedPool and NonPagedPool are selected once when the pool is created. */
    address = Allocator_Malloc(
        (BOOLEAN)(pool_type == (ULONG)MEMPOOL_NONPAGED),
        (size_t)size, tag);
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

static MEMPOOL_PAGE *mempool_alloc_page(POOL *pool, ULONG pool_type, ULONG tag)
{
    MEMPOOL_PAGE *page = (MEMPOOL_PAGE *)0;
    UCHAR *bitmap = (UCHAR *)0;

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
    if (pool != (POOL *)0) {
        /* New pages inherit the bits that are permanently unavailable for
           cells (bitmap padding).  The first page additionally marks POOL
           cells after the pool object has been initialized. */
        bitmap = (UCHAR *)page + MEMPOOL_PAGE_HEADER_SIZE;
        mempool_copy(bitmap, pool->initial_bitmap,
                     (SIZE_T)MEMPOOL_PAGE_BITMAP_SIZE);
        List_Insert_Before(&pool->pages, (void *)0, page);
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

    /* Step 3: place the pool object at the beginning of the data area. */
    pool = (POOL *)(bitmap + MEMPOOL_PAGE_BITMAP_SIZE);
    page->pool = pool;
    mempool_zero(pool, (SIZE_T)sizeof(POOL));
    pool->eyecatcher = tag;
    pool->pool_type = pool_type;
#ifdef _KERNEL_MODE
    ExInitializePushLock(&pool->lock);
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
    List_Insert_Before(&pool->pages, (void *)0, page);

    /* Step 5: reserve the cells occupied by POOL in the first page. */
    pool_cells = MEMPOOL_NUM_CELLS(sizeof(POOL));
    i = 0;
    while (i < pool_cells) {
        byte_index = i / 8UL;
        bit = (UCHAR)(1U << (i & 7UL));
        bitmap[byte_index] |= bit;
        ++i;
    }
    page->num_free = (USHORT)(MEMPOOL_NUM_PAGE_CELLS - pool_cells);
    page->num_used = (USHORT)pool_cells;
    /* Reserving POOL changed the bitmap, so compute a fresh run lazily. */
    page->max_free_run = 0;
    return pool;
}

MEMPOOL *Mempool_CreatePool(MEMPOOL_TYPE type)
{
    MEMPOOL_REGISTRATION *registration = (MEMPOOL_REGISTRATION *)0;
    POOL *pool = (POOL *)0;

    /* Keep the public type check here so invalid enum values never reach the
       platform-specific allocation routine. */
    if (type != MEMPOOL_PAGED && type != MEMPOOL_NONPAGED)
        return (MEMPOOL *)0;
    /* The registration outlives the pool object and lets later calls reject a
       stale opaque handle by address comparison without dereferencing it. */
    registration = (MEMPOOL_REGISTRATION *)mempool_registry_alloc();
    if (registration == (MEMPOOL_REGISTRATION *)0)
        return (MEMPOOL *)0;
    pool = mempool_create((ULONG)type, MEMPOOL_POOL_TAG);
    if (pool == (POOL *)0) {
        mempool_registry_free(registration);
        return (MEMPOOL *)0;
    }
    registration->pool = pool;
    registration->next = (MEMPOOL_REGISTRATION *)0;
    mempool_registry_add(registration);
    return pool;
}

ULONG Mempool_DestroyPool(MEMPOOL *pool)
{
    MEMPOOL_LARGE_CHUNK *large_chunk = (MEMPOOL_LARGE_CHUNK *)0;
    MEMPOOL_LARGE_CHUNK *next_large_chunk = (MEMPOOL_LARGE_CHUNK *)0;
    MEMPOOL_PAGE *page = (MEMPOOL_PAGE *)0;
    MEMPOOL_PAGE *next_page = (MEMPOOL_PAGE *)0;
    MEMPOOL_PAGE *pool_page = (MEMPOOL_PAGE *)0;
    ULONG page_count = 0;
    MEMPOOL_REGISTRATION *registration = (MEMPOOL_REGISTRATION *)0;

    /* The writer gate removes the registration before the first page (which
       contains this lock and lifecycle state) can be released. */
    if (pool == (POOL *)0)
        return 0;
    mempool_lifecycle_gate_exclusive_enter();
    registration = mempool_registry_find_locked(pool);
    if (registration == (MEMPOOL_REGISTRATION *)0 ||
        !mempool_begin_destroy(pool)) {
        mempool_lifecycle_gate_exclusive_leave();
        return 0;
    }
    mempool_registry_remove_locked(registration);
    mempool_lifecycle_gate_exclusive_leave();
    page_count = 0;
    /* Large chunks own their OS allocation, so free them before pages. */
    large_chunk = (MEMPOOL_LARGE_CHUNK *)List_Head(&pool->large_chunks);
    while (large_chunk != (MEMPOOL_LARGE_CHUNK *)0) {
        next_large_chunk = (MEMPOOL_LARGE_CHUNK *)List_Next(large_chunk);
        page_count += large_chunk->size / MEMPOOL_PAGE_SIZE;
        mempool_free_mem(large_chunk->ptr, large_chunk->eyecatcher);
        large_chunk = next_large_chunk;
    }

    /* The page containing POOL is released last because it contains the
       lock, lists, and pool metadata currently being traversed. */
    pool_page = (MEMPOOL_PAGE *)((ULONG_PTR)pool & ~MEMPOOL_PAGE_MASK);
    page = (MEMPOOL_PAGE *)List_Head(&pool->pages);
    while (page != (MEMPOOL_PAGE *)0) {
        next_page = (MEMPOOL_PAGE *)List_Next(page);
        if (page != pool_page)
            mempool_free_mem(page, page->eyecatcher);
        ++page_count;
        page = next_page;
    }
    page = (MEMPOOL_PAGE *)List_Head(&pool->full_pages);
    while (page != (MEMPOOL_PAGE *)0) {
        next_page = (MEMPOOL_PAGE *)List_Next(page);
        if (page != pool_page)
            mempool_free_mem(page, page->eyecatcher);
        ++page_count;
        page = next_page;
    }

#ifdef _KERNEL_MODE
    /* EX_PUSH_LOCK is embedded in the pool and needs no separate cleanup. */
#else
    DeleteCriticalSection(&pool->lock);
#endif
    mempool_free_mem(pool_page, pool_page->eyecatcher);
    /* No caller can find this registration now; free it after all pool pages
       are gone, while the writer gate closes the tiny final admission window. */
    mempool_lifecycle_gate_exclusive_enter();
    mempool_registry_free(registration);
    mempool_lifecycle_gate_exclusive_leave();
    return page_count;
}

void *Mempool_Alloc(MEMPOOL *pool, ULONG size)
{
    void *address = (void *)0;
    void *payload = (void *)0;
    ULONG total_size = 0;
    ULONG cell_count = 0;
    int operation_entered = 0;
#if MEMPOOL_ZERO_ON_ALLOC
    SIZE_T clear_size = 0;
#endif

    /* Store the internal size, not the caller's size, because the allocator
       rounds requests to whole cells and uses that value during Free.  The
       complete hidden header is two pointer widths: this keeps the payload
       naturally 8-byte aligned on x86 and 16-byte aligned on x64/ARM64.
       Only the first ULONG stores the size; the remaining bytes are padding. */
    if (pool == (POOL *)0 || size == 0)
        return (void *)0;
    /* The hidden size word is part of every internal allocation. */
    /* Reject requests that could overflow either the hidden size word or
       the later page-rounding arithmetic. */
    if (size > MEMPOOL_MAX_ALLOC_SIZE)
        return (void *)0;
    if (!mempool_operation_admit(pool)) {
        return (void *)0;
    }
    operation_entered = 1;
    total_size = size + (ULONG)MEMPOOL_ALLOCATION_HEADER_SIZE;
    /* A large request bypasses the bitmap and receives page-granular memory. */
    if (total_size > MEMPOOL_LARGE_CHUNK_MINIMUM) {
        address = mempool_get_large_chunk(pool, total_size);
#if MEMPOOL_ZERO_ON_ALLOC
        if (address != (void *)0)
            clear_size = (SIZE_T)((total_size + MEMPOOL_LARGE_CHUNK_SIZE +
                         MEMPOOL_PAGE_SIZE - 1UL) &
                        ~(MEMPOOL_PAGE_SIZE - 1UL));
        if (clear_size >= (SIZE_T)MEMPOOL_LARGE_CHUNK_SIZE +
                          (SIZE_T)MEMPOOL_ALLOCATION_HEADER_SIZE)
            clear_size -= (SIZE_T)MEMPOOL_LARGE_CHUNK_SIZE +
                          (SIZE_T)MEMPOOL_ALLOCATION_HEADER_SIZE;
        else
            clear_size = 0;
#endif
    } else {
        cell_count = MEMPOOL_NUM_CELLS(total_size);
        address = mempool_get_cells(pool, cell_count);
#if MEMPOOL_ZERO_ON_ALLOC
        clear_size = (SIZE_T)cell_count * (SIZE_T)MEMPOOL_CELL_SIZE -
                     (SIZE_T)MEMPOOL_ALLOCATION_HEADER_SIZE;
#endif
    }
    if (address != (void *)0) {
        *(ULONG *)address = total_size;
        *((POOL **)((UCHAR *)address + sizeof(ULONG_PTR))) = pool;
        payload = (UCHAR *)address + MEMPOOL_ALLOCATION_HEADER_SIZE;
#if MEMPOOL_ZERO_ON_ALLOC
        /* Clear the complete rounded capacity, not only the requested prefix:
           otherwise a later larger allocation could inherit a former
           allocation's tail bytes from the same cell run. */
        mempool_zero(payload, clear_size);
#endif
    }
    if (operation_entered != 0)
        mempool_operation_leave(pool);
    address = payload;
    return address;
}

void Mempool_Free(void *address)
{
    ULONG total_size = 0;
    ULONG large_size = 0;
    void *base_address = (void *)0;
    POOL *pool = (POOL *)0;
    MEMPOOL_PAGE *page = (MEMPOOL_PAGE *)0;
    MEMPOOL_LARGE_CHUNK *chunk = (MEMPOOL_LARGE_CHUNK *)0;
    int metadata_valid = 0;
    int operation_entered = 0;

    if (address == (void *)0) {
        mempool_abend(MEMPOOL_ABEND_FREE_NULL);
        return;
    }
    if ((ULONG_PTR)address < (ULONG_PTR)MEMPOOL_ALLOCATION_HEADER_SIZE) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_RANGE);
        return;
    }
    /* Move back over the complete hidden header before classifying the
       allocation.  The size word is the first ULONG in that header. */
    base_address = (UCHAR *)address - MEMPOOL_ALLOCATION_HEADER_SIZE;
    /* Keep the shared admission gate while reading both the owner slot and
       the page/chunk owner.  This prevents DestroyPool from freeing the
       backing page between those reads and operation admission. */
    mempool_lifecycle_gate_shared_enter();
    pool = *((POOL **)((UCHAR *)base_address + sizeof(ULONG_PTR)));
    if (pool == (POOL *)0 || mempool_registry_find_locked(pool) ==
            (MEMPOOL_REGISTRATION *)0 || !mempool_operation_enter(pool)) {
        mempool_lifecycle_gate_shared_leave();
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_HEADER);
        return;
    }
    operation_entered = 1;
    total_size = *(ULONG *)base_address;
    if (total_size < (ULONG)MEMPOOL_ALLOCATION_HEADER_SIZE) {
        mempool_lifecycle_gate_shared_leave();
        mempool_abend(MEMPOOL_ABEND_FREE_SIZE_MISMATCH);
        mempool_operation_leave(pool);
        return;
    }
    /* OS page alignment is the discriminator: only large chunks start at a
       page boundary; cell data starts after the page header and bitmap. */
    if (((ULONG_PTR)base_address & MEMPOOL_PAGE_MASK) == 0) {
        if (total_size <= MEMPOOL_LARGE_CHUNK_MAXIMUM) {
            large_size = (total_size + MEMPOOL_LARGE_CHUNK_SIZE +
                          MEMPOOL_PAGE_SIZE - 1UL) &
                         ~(MEMPOOL_PAGE_SIZE - 1UL);
            chunk = (MEMPOOL_LARGE_CHUNK *)((UCHAR *)base_address +
                     large_size - MEMPOOL_LARGE_CHUNK_SIZE);
            if (chunk->pool == pool && chunk->ptr == base_address &&
                chunk->size == large_size &&
                chunk->eyecatcher == pool->eyecatcher)
                metadata_valid = 1;
        }
    } else {
        page = (MEMPOOL_PAGE *)((ULONG_PTR)base_address & ~MEMPOOL_PAGE_MASK);
        if (page->pool == pool && page->eyecatcher == pool->eyecatcher)
            metadata_valid = 1;
    }
    mempool_lifecycle_gate_shared_leave();
    if (!metadata_valid) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_HEADER);
        mempool_operation_leave(pool);
        return;
    }
    if (((ULONG_PTR)base_address & MEMPOOL_PAGE_MASK) == 0)
        mempool_free_large_chunk(base_address, total_size);
    else
        mempool_free_cells(base_address, MEMPOOL_NUM_CELLS(total_size));
    if (operation_entered != 0)
        mempool_operation_leave(pool);
}

/* ------------------------------------------------------------------------- */
/* Cell allocator                                                            */
/* ------------------------------------------------------------------------- */

static ULONG mempool_find_cells(MEMPOOL_PAGE *page, ULONG cell_count,
                                ULONG *largest_run)
{
    UCHAR *bitmap = (UCHAR *)0;
    ULONG i = 0;
    ULONG run_start = 0;
    ULONG run_length = 0;
    ULONG maximum = 0;
    ULONG byte_index = 0;
    UCHAR bit_mask = 0;
    UCHAR byte_value = 0;
    ULONG found = (ULONG)-1;

    /*
     * Find the first run of cell_count zero bits.  A zero bit is free and a
     * one bit is either allocated or permanently reserved.  This scan also
     * computes the exact largest run.  That value is cached after a failed
     * search so a fragmented page is not repeatedly scanned for impossible
     * requests.  The page contains at most a few hundred cells, making this
     * bounded bit walk cheaper and less error-prone than maintaining a second
     * free-run index on every mutation.  Full 0x00/0xFF bytes are still skipped
     * in one step, preserving the hot-path optimization from pool.c.
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
                if (run_length > maximum)
                    maximum = run_length;
                if (found == (ULONG)-1 && run_length >= cell_count)
                    found = run_start;
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
            if (run_length > maximum)
                maximum = run_length;
            if (found == (ULONG)-1 && run_length >= cell_count)
                found = run_start;
        } else {
            run_length = 0;
        }
        ++i;
    }
    if (largest_run != (ULONG *)0)
        *largest_run = maximum;
    return found;
}

static void *mempool_get_cells(POOL *pool, ULONG cell_count)
{
    MEMPOOL_PAGE *page = (MEMPOOL_PAGE *)0;
    MEMPOOL_PAGE *next_page = (MEMPOOL_PAGE *)0;
    UCHAR *bitmap = (UCHAR *)0;
    ULONG index = 0;
    ULONG largest_run = 0;
    ULONG mask = 0;
    ULONG remaining = 0;
    UCHAR *address = (UCHAR *)0;
    MEMPOOL_LOCK_DECL

    /* num_free is an exact count; max_free_run is an exact cached filter after
       a failed scan, and zero means the bitmap must be scanned. */
    if (cell_count == 0 || cell_count > MEMPOOL_NUM_PAGE_CELLS)
        return (void *)0;
    MEMPOOL_LOCK(pool);
    page = (MEMPOOL_PAGE *)List_Head(&pool->pages);
    /* Search usable pages first.  Full pages are deliberately not traversed. */
    while (page != (MEMPOOL_PAGE *)0) {
        next_page = (MEMPOOL_PAGE *)List_Next(page);
        if (page->eyecatcher != pool->eyecatcher) {
            mempool_abend(MEMPOOL_ABEND_GET_CELLS_HEADER);
            MEMPOOL_UNLOCK(pool);
            return (void *)0;
        }
        if (page->num_free >= cell_count &&
            (page->max_free_run == 0 || page->max_free_run >= cell_count)) {
            largest_run = 0;
            index = mempool_find_cells(page, cell_count, &largest_run);
            if (index != (ULONG)-1)
                break;
            /* Fragmentation, rather than lack of total free cells, explains
               the miss.  Preserve the exact count and cache only the largest
               contiguous run; smaller requests can still use this page. */
            page->max_free_run = (USHORT)largest_run;
        }
        page = next_page;
    }
    if (page == (MEMPOOL_PAGE *)0) {
        /* No usable run exists, so allocate and link a fresh page. */
        page = mempool_alloc_page(pool, pool->pool_type, pool->eyecatcher);
        if (page == (MEMPOOL_PAGE *)0) {
            MEMPOOL_UNLOCK(pool);
            return (void *)0;
        }
        index = 0;
    }
    /* Reserve the cells in both counters and then in the bitmap.  num_free is
       only a search hint; num_used is exact and lets Free reclaim an empty
       page without scanning every bitmap bit. */
    page->num_free = (USHORT)(page->num_free - cell_count);
    page->num_used = (USHORT)(page->num_used + cell_count);
    page->max_free_run = 0;
    if (page->num_free < MEMPOOL_FULL_PAGE_THRESHOLD) {
        /* Keep low-capacity pages out of future linear searches. */
        List_Remove(&pool->pages, page);
        List_Insert_Before(&pool->full_pages, (void *)0, page);
    }
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
    address = (UCHAR *)page + MEMPOOL_PAGE_HEADER_SIZE +
              MEMPOOL_PAGE_BITMAP_SIZE + index * MEMPOOL_CELL_SIZE;
    MEMPOOL_UNLOCK(pool);
    return address;
}

static void mempool_free_cells(void *address, ULONG cell_count)
{
    MEMPOOL_PAGE *page = (MEMPOOL_PAGE *)0;
    POOL *pool = (POOL *)0;
    UCHAR *bitmap = (UCHAR *)0;
    ULONG offset = 0;
    ULONG index = 0;
    ULONG i = 0;
    ULONG byte_index = 0;
    ULONG bit_mask = 0;
    ULONG mask = 0;
    ULONG remaining = 0;
    int page_was_full = 0;
    int page_moved_to_pages = 0;
    int reclaim_page = 0;
    MEMPOOL_PAGE *release_page = (MEMPOOL_PAGE *)0;
    ULONG release_tag = 0;
    MEMPOOL_LOCK_DECL

    /* cell_count comes from the hidden size word read by Mempool_Free. */
    if (address == (void *)0 || cell_count == 0)
        return;
    /* The page and cell index can be recovered from the aligned cell address;
       no allocation table is needed. */
    page = (MEMPOOL_PAGE *)((ULONG_PTR)address & ~MEMPOOL_PAGE_MASK);
    offset = (ULONG)((ULONG_PTR)address & MEMPOOL_PAGE_MASK);
    if (offset < MEMPOOL_PAGE_HEADER_SIZE + MEMPOOL_PAGE_BITMAP_SIZE ||
        offset >= MEMPOOL_PAGE_SIZE) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_RANGE);
        return;
    }
    if ((offset - MEMPOOL_PAGE_HEADER_SIZE - MEMPOOL_PAGE_BITMAP_SIZE) %
        MEMPOOL_CELL_SIZE != 0) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_RANGE);
        return;
    }
    index = (ULONG)((offset - MEMPOOL_PAGE_HEADER_SIZE -
                     MEMPOOL_PAGE_BITMAP_SIZE) / MEMPOOL_CELL_SIZE);
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
    bitmap = (UCHAR *)page + MEMPOOL_PAGE_HEADER_SIZE;
    /* Check before clearing so a double free is reported rather than silently
       inflating either accounting counter. */
    for (i = 0; i < cell_count; ++i) {
        byte_index = (index + i) / 8UL;
        bit_mask = 1UL << ((index + i) & 7UL);
        if ((bitmap[byte_index] & (UCHAR)bit_mask) == 0) {
            mempool_abend(MEMPOOL_ABEND_FREE_CELLS_DOUBLE);
            MEMPOOL_UNLOCK(pool);
            return;
        }
    }
    /* Remember which list owns the page before changing the counters.  A page
       that becomes completely empty is removed instead of being moved back to
       pages and retained forever.  The first page cannot be reclaimed because
       it contains POOL itself and its synchronization object. */
    page_was_full = page->num_free < MEMPOOL_FULL_PAGE_THRESHOLD;
    if (page->num_used < cell_count) {
        mempool_abend(MEMPOOL_ABEND_FREE_CELLS_COUNT);
        MEMPOOL_UNLOCK(pool);
        return;
    }
    reclaim_page = page !=
                   (MEMPOOL_PAGE *)((ULONG_PTR)pool & ~MEMPOOL_PAGE_MASK) &&
                   page->num_used == (USHORT)cell_count;
    if (reclaim_page == 0 && page_was_full != 0 &&
        page->num_free + cell_count >= MEMPOOL_FULL_PAGE_THRESHOLD) {
        /* A free operation made this page useful for new allocations again. */
        List_Remove(&pool->full_pages, page);
        List_Insert_Before(&pool->pages, (void *)0, page);
        page_moved_to_pages = 1;
    }
    page->num_free = (USHORT)(page->num_free + cell_count);
    page->num_used = (USHORT)(page->num_used - cell_count);
    /* Any bitmap mutation invalidates the cached maximum run. */
    page->max_free_run = 0;
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
    if (reclaim_page != 0) {
        if (page_moved_to_pages != 0 || page_was_full == 0)
            List_Remove(&pool->pages, page);
        else
            List_Remove(&pool->full_pages, page);
        release_page = page;
        release_tag = page->eyecatcher;
    }
    MEMPOOL_UNLOCK(pool);

    /* Do not call the OS allocator while holding the pool lock.  The page is
       no longer reachable through either list, so its backing allocation can
       be released after the protected state transition is complete. */
    if (release_page != (MEMPOOL_PAGE *)0)
        mempool_free_mem(release_page, release_tag);
}

/* ------------------------------------------------------------------------- */
/* Large chunk allocator                                                     */
/* ------------------------------------------------------------------------- */

static void *mempool_get_large_chunk(POOL *pool, ULONG size)
{
    ULONG large_size = 0;
    void *address = (void *)0;
    MEMPOOL_LARGE_CHUNK *chunk = (MEMPOOL_LARGE_CHUNK *)0;
    MEMPOOL_LOCK_DECL

    /* Layout of a large allocation:
       [size word + caller bytes ........ padding][LARGE_CHUNK descriptor] */
    if (size > MEMPOOL_LARGE_CHUNK_MAXIMUM)
        return (void *)0;
    large_size = (size + MEMPOOL_LARGE_CHUNK_SIZE + MEMPOOL_PAGE_SIZE - 1UL) &
                 ~(MEMPOOL_PAGE_SIZE - 1UL);
    address = mempool_alloc_mem(pool->pool_type, large_size, pool->eyecatcher);
    if (address == (void *)0)
        return (void *)0;
    /* Keep the descriptor inside the allocation so it is freed atomically
       with the caller's memory and needs no separate metadata allocation. */
    chunk = (MEMPOOL_LARGE_CHUNK *)((UCHAR *)address + large_size -
                                    MEMPOOL_LARGE_CHUNK_SIZE);
    chunk->eyecatcher = pool->eyecatcher;
    chunk->pool = pool;
    chunk->ptr = address;
    chunk->size = large_size;
    /* The descriptor is visible to DestroyPool only after it is linked. */
    MEMPOOL_LOCK(pool);
    List_Insert_Before(&pool->large_chunks, (void *)0, chunk);
    MEMPOOL_UNLOCK(pool);
    return address;
}

static void mempool_free_large_chunk(void *address, ULONG size)
{
    ULONG large_size = 0;
    MEMPOOL_LARGE_CHUNK *chunk = (MEMPOOL_LARGE_CHUNK *)0;
    POOL *pool = (POOL *)0;
    MEMPOOL_LOCK_DECL

    /* Recompute the same rounded size used by GetLargeChunk to locate the
       descriptor at the end of the allocation. */
    if (size > MEMPOOL_LARGE_CHUNK_MAXIMUM) {
        mempool_abend(MEMPOOL_ABEND_FREE_SIZE_MISMATCH);
        return;
    }
    large_size = (size + MEMPOOL_LARGE_CHUNK_SIZE + MEMPOOL_PAGE_SIZE - 1UL) &
                 ~(MEMPOOL_PAGE_SIZE - 1UL);
    chunk = (MEMPOOL_LARGE_CHUNK *)((UCHAR *)address + large_size -
                                    MEMPOOL_LARGE_CHUNK_SIZE);
    pool = chunk->pool;
    if (pool == (POOL *)0 || chunk->eyecatcher != pool->eyecatcher ||
        chunk->ptr != address || chunk->size != large_size) {
        mempool_abend(MEMPOOL_ABEND_LARGE_HEADER);
        return;
    }
    /* Unlink before releasing the backing allocation so normal pool
       operations never observe a descriptor whose memory is already gone. */
    MEMPOOL_LOCK(pool);
    List_Remove(&pool->large_chunks, chunk);
    MEMPOOL_UNLOCK(pool);
    mempool_free_mem(address, chunk->eyecatcher);
}
