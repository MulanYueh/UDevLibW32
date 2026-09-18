/*
 * Catch2 regression tests for the public mempool API.
 *
 * Build the C implementation as C and link this translation unit as C++:
 *
 *   gcc -std=c11 -D_WIN32 -c allocator.c mempool.c libc.c
 *   g++ -std=c++17 -I. mempool_unit_test.cpp allocator.o mempool.o libc.o \
 *       -o mempool_unit_test.exe
 *
 * This file is intentionally independent from the other unit-test runners.
 */

#define CATCH_CONFIG_MAIN
#include "catch2/catch.hpp"

#include "mempool.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace
{

/* Private-layout mirrors used only for corruption/fail-closed regression
 * tests.  Production users continue to see MEMPOOL as an opaque type. */
struct WhiteboxRegistration;

struct WhiteboxBacking
{
    WhiteboxBacking *hash_next;
    WhiteboxBacking *identity_next;
    WhiteboxBacking *owner_next;
    WhiteboxBacking *owner_prev;
    void *base;
    void *object;
    WhiteboxRegistration *registration;
    ULONG size;
    ULONG tag;
    ULONG kind;
    volatile LONG references;
    volatile LONG closing;
};

struct WhiteboxRegistration
{
    MEMPOOL *pool;
    ULONG pool_type;
    volatile LONG lifecycle_state;
    WhiteboxBacking *backings;
    ULONG backing_count;
    WhiteboxRegistration *next;
};

struct WhiteboxListElement
{
    WhiteboxListElement *next;
    WhiteboxListElement *prev;
};

struct WhiteboxList
{
    WhiteboxListElement *head;
    WhiteboxListElement *tail;
    int count;
};

struct WhiteboxPage
{
    WhiteboxListElement list_elem;
    WhiteboxBacking backing;
    WhiteboxPage *bin_next;
    WhiteboxPage *bin_prev;
    WhiteboxPage *next;
    MEMPOOL *pool;
    ULONG eyecatcher;
    USHORT num_free;
    USHORT num_used;
    USHORT max_free_run;
};

struct WhiteboxLargeChunk
{
    WhiteboxListElement list_elem;
    WhiteboxBacking backing;
    ULONG eyecatcher;
    MEMPOOL *pool;
    void *ptr;
    ULONG size;
    ULONG allocation_size;
    ULONG cookie;
};

constexpr std::size_t WHITEBOX_PAGE_SIZE = 65536U;
constexpr std::size_t WHITEBOX_CELL_SIZE = 128U;
constexpr std::size_t whitebox_pad_cell(std::size_t value)
{
    return (value + WHITEBOX_CELL_SIZE - 1U) &
           ~(WHITEBOX_CELL_SIZE - 1U);
}
constexpr std::size_t WHITEBOX_PAGE_HEADER_SIZE =
    whitebox_pad_cell(sizeof(WhiteboxPage));
constexpr std::size_t WHITEBOX_RAW_CELL_COUNT =
    (WHITEBOX_PAGE_SIZE - WHITEBOX_PAGE_HEADER_SIZE) / WHITEBOX_CELL_SIZE;
constexpr std::size_t WHITEBOX_PAGE_BITMAP_SIZE =
    whitebox_pad_cell((WHITEBOX_RAW_CELL_COUNT + 7U) / 8U);
constexpr std::size_t WHITEBOX_PAGE_LENGTHS_SIZE =
    whitebox_pad_cell(WHITEBOX_RAW_CELL_COUNT * sizeof(USHORT));
constexpr std::size_t WHITEBOX_PAGE_DATA_OFFSET =
    WHITEBOX_PAGE_HEADER_SIZE + WHITEBOX_PAGE_BITMAP_SIZE +
    WHITEBOX_PAGE_LENGTHS_SIZE;
constexpr std::size_t WHITEBOX_NUM_PAGE_CELLS =
    (WHITEBOX_PAGE_SIZE - WHITEBOX_PAGE_DATA_OFFSET) / WHITEBOX_CELL_SIZE;
constexpr std::size_t WHITEBOX_RUN_BIN_COUNT =
    WHITEBOX_NUM_PAGE_CELLS + 1U;

struct WhiteboxPool
{
    ULONG eyecatcher;
    ULONG pool_type;
    WhiteboxRegistration *registration;
    CRITICAL_SECTION lock;
    ULONG cookie_seed;
    WhiteboxList pages;
    WhiteboxList full_pages;
    WhiteboxList large_chunks;
    ULONG large_chunk_count;
    ULONG large_chunk_count_check;
    WhiteboxPage *run_bins[WHITEBOX_RUN_BIN_COUNT];
    USHORT run_bin_counts[WHITEBOX_RUN_BIN_COUNT];
    USHORT run_bin_checks[WHITEBOX_RUN_BIN_COUNT];
    ULONG page_count;
    ULONG page_count_check;
    unsigned char initial_bitmap[WHITEBOX_PAGE_BITMAP_SIZE];
};

static_assert(offsetof(WhiteboxPage, backing) == 2U * sizeof(void *),
              "white-box page mirror must match mempool.c");
static_assert(whitebox_pad_cell(sizeof(WhiteboxPool)) <=
                  WHITEBOX_NUM_PAGE_CELLS * WHITEBOX_CELL_SIZE,
              "white-box pool mirror must fit in its owner page");

struct WhiteboxSnapshot
{
    WhiteboxBacking *backing;
    WhiteboxBacking *hash_next;
    WhiteboxBacking *identity_next;
    WhiteboxBacking *owner_next;
    WhiteboxBacking *owner_prev;
    LONG references;
    LONG closing;
};

static volatile LONG whitebox_breakpoints = 0;

static LONG CALLBACK whitebox_exception_handler(EXCEPTION_POINTERS *info)
{
    if (info != static_cast<EXCEPTION_POINTERS *>(0) &&
        info->ExceptionRecord != static_cast<EXCEPTION_RECORD *>(0) &&
        info->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        (void)InterlockedIncrement(&whitebox_breakpoints);
#if defined(_M_X64) || defined(__x86_64__)
        ++info->ContextRecord->Rip;
#elif defined(_M_IX86) || defined(__i386__)
        ++info->ContextRecord->Eip;
#elif defined(_M_ARM64) || defined(__aarch64__)
        info->ContextRecord->Pc += 4;
#endif
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

struct BreakpointScope
{
    PVOID handle;

    BreakpointScope() : handle(static_cast<PVOID>(0))
    {
        handle = AddVectoredExceptionHandler(1UL, whitebox_exception_handler);
    }

    ~BreakpointScope()
    {
        if (handle != static_cast<PVOID>(0))
            (void)RemoveVectoredExceptionHandler(handle);
    }

    BreakpointScope(const BreakpointScope &) = delete;
    BreakpointScope &operator=(const BreakpointScope &) = delete;
};

static WhiteboxBacking *whitebox_pool_backing(MEMPOOL *pool)
{
    const std::uintptr_t page_mask = static_cast<std::uintptr_t>(65536U - 1U);
    const std::uintptr_t pool_value =
        reinterpret_cast<std::uintptr_t>(pool);
    unsigned char *page = reinterpret_cast<unsigned char *>(
        pool_value & ~page_mask);

    return reinterpret_cast<WhiteboxBacking *>(page + 2U * sizeof(void *));
}

static WhiteboxRegistration *whitebox_registration(MEMPOOL *pool)
{
    return whitebox_pool_backing(pool)->registration;
}

static WhiteboxBacking *whitebox_find_backing(MEMPOOL *pool, void *allocation)
{
    const std::size_t header_alignment = 2U * sizeof(void *);
    const std::size_t raw_header_size = 2U * sizeof(ULONG) + sizeof(void *);
    const std::size_t header_size =
        (raw_header_size + header_alignment - 1U) & ~(header_alignment - 1U);
    WhiteboxRegistration *registration = whitebox_registration(pool);
    WhiteboxBacking *backing = registration->backings;
    void *base = static_cast<unsigned char *>(allocation) - header_size;
    ULONG index = 0UL;

    while (backing != static_cast<WhiteboxBacking *>(0) &&
           index < registration->backing_count) {
        if (backing->base == base)
            return backing;
        backing = backing->owner_next;
        ++index;
    }
    return static_cast<WhiteboxBacking *>(0);
}

static WhiteboxPage *whitebox_page_from_pointer(const void *pointer)
{
    const std::uintptr_t mask =
        static_cast<std::uintptr_t>(WHITEBOX_PAGE_SIZE - 1U);
    const std::uintptr_t value =
        reinterpret_cast<std::uintptr_t>(pointer);

    return reinterpret_cast<WhiteboxPage *>(value & ~mask);
}

static WhiteboxPool *whitebox_pool(MEMPOOL *pool)
{
    return reinterpret_cast<WhiteboxPool *>(pool);
}

struct WhiteboxPageIndexSnapshot
{
    WhiteboxList pages;
    WhiteboxList full_pages;
    std::array<WhiteboxPage *, WHITEBOX_RUN_BIN_COUNT> run_bins;
    std::array<USHORT, WHITEBOX_RUN_BIN_COUNT> run_bin_counts;
    std::array<USHORT, WHITEBOX_RUN_BIN_COUNT> run_bin_checks;
    ULONG large_chunk_count;
    ULONG large_chunk_count_check;
    ULONG page_count;
    ULONG page_count_check;
};

struct WhiteboxPageHeaderSnapshot
{
    WhiteboxPage *page;
    std::array<unsigned char, sizeof(WhiteboxPage)> bytes;
};

static WhiteboxPageIndexSnapshot whitebox_snapshot_page_index(MEMPOOL *pool)
{
    WhiteboxPool *private_pool = whitebox_pool(pool);
    WhiteboxPageIndexSnapshot snapshot = {};

    snapshot.pages = private_pool->pages;
    snapshot.full_pages = private_pool->full_pages;
    std::memcpy(snapshot.run_bins.data(), private_pool->run_bins,
                sizeof(private_pool->run_bins));
    std::memcpy(snapshot.run_bin_counts.data(),
                private_pool->run_bin_counts,
                sizeof(private_pool->run_bin_counts));
    std::memcpy(snapshot.run_bin_checks.data(),
                private_pool->run_bin_checks,
                sizeof(private_pool->run_bin_checks));
    snapshot.large_chunk_count = private_pool->large_chunk_count;
    snapshot.large_chunk_count_check = private_pool->large_chunk_count_check;
    snapshot.page_count = private_pool->page_count;
    snapshot.page_count_check = private_pool->page_count_check;
    return snapshot;
}

static bool whitebox_page_index_unchanged(
    MEMPOOL *pool, const WhiteboxPageIndexSnapshot &snapshot)
{
    WhiteboxPool *private_pool = whitebox_pool(pool);

    return std::memcmp(&private_pool->pages, &snapshot.pages,
                       sizeof(snapshot.pages)) == 0 &&
           std::memcmp(&private_pool->full_pages, &snapshot.full_pages,
                       sizeof(snapshot.full_pages)) == 0 &&
           std::memcmp(private_pool->run_bins, snapshot.run_bins.data(),
                       sizeof(private_pool->run_bins)) == 0 &&
           std::memcmp(private_pool->run_bin_counts,
                       snapshot.run_bin_counts.data(),
                       sizeof(private_pool->run_bin_counts)) == 0 &&
           std::memcmp(private_pool->run_bin_checks,
                       snapshot.run_bin_checks.data(),
                       sizeof(private_pool->run_bin_checks)) == 0 &&
           private_pool->large_chunk_count == snapshot.large_chunk_count &&
           private_pool->large_chunk_count_check ==
               snapshot.large_chunk_count_check &&
           private_pool->page_count == snapshot.page_count &&
           private_pool->page_count_check == snapshot.page_count_check;
}

static std::vector<WhiteboxPageHeaderSnapshot>
whitebox_snapshot_page_headers(WhiteboxRegistration *registration)
{
    std::vector<WhiteboxPageHeaderSnapshot> snapshots;
    WhiteboxBacking *backing = registration->backings;
    ULONG index = 0UL;

    while (backing != static_cast<WhiteboxBacking *>(0) &&
           index < registration->backing_count) {
        if (backing->kind == 1UL &&
            backing->size == static_cast<ULONG>(WHITEBOX_PAGE_SIZE)) {
            WhiteboxPageHeaderSnapshot snapshot = {};
            snapshot.page = static_cast<WhiteboxPage *>(backing->object);
            if (snapshot.page != static_cast<WhiteboxPage *>(0)) {
                std::memcpy(snapshot.bytes.data(), snapshot.page,
                            sizeof(WhiteboxPage));
                snapshots.push_back(snapshot);
            }
        }
        backing = backing->owner_next;
        ++index;
    }
    return snapshots;
}

static bool whitebox_page_headers_unchanged(
    const std::vector<WhiteboxPageHeaderSnapshot> &snapshots)
{
    for (std::size_t index = 0U; index < snapshots.size(); ++index) {
        if (std::memcmp(snapshots[index].page,
                        snapshots[index].bytes.data(),
                        sizeof(WhiteboxPage)) != 0)
            return false;
    }
    return true;
}

static ULONG whitebox_pointer_bucket(void *pointer)
{
    std::uintptr_t value = reinterpret_cast<std::uintptr_t>(pointer) / 65536U;

    value ^= value >> 7U;
#if defined(_WIN64) || defined(_M_AMD64) || defined(_M_ARM64) || \
    defined(__x86_64__) || defined(__aarch64__)
    value ^= value >> 32U;
#endif
    return static_cast<ULONG>(value) & 255UL;
}

static std::vector<WhiteboxSnapshot> whitebox_snapshot_chain(
    WhiteboxRegistration *registration, ULONG expected_count)
{
    std::vector<WhiteboxSnapshot> snapshots;
    WhiteboxBacking *backing = registration->backings;
    ULONG index = 0UL;

    snapshots.reserve(static_cast<std::size_t>(expected_count));
    while (backing != static_cast<WhiteboxBacking *>(0) &&
           index < expected_count) {
        WhiteboxSnapshot snapshot = {};
        snapshot.backing = backing;
        snapshot.hash_next = backing->hash_next;
        snapshot.identity_next = backing->identity_next;
        snapshot.owner_next = backing->owner_next;
        snapshot.owner_prev = backing->owner_prev;
        snapshot.references = backing->references;
        snapshot.closing = backing->closing;
        snapshots.push_back(snapshot);
        backing = backing->owner_next;
        ++index;
    }
    return snapshots;
}

static bool whitebox_chain_unchanged(
    const std::vector<WhiteboxSnapshot> &snapshots)
{
    std::size_t index = 0U;

    while (index < snapshots.size()) {
        const WhiteboxSnapshot &snapshot = snapshots[index];
        if (snapshot.backing->hash_next != snapshot.hash_next ||
            snapshot.backing->identity_next != snapshot.identity_next ||
            snapshot.backing->owner_next != snapshot.owner_next ||
            snapshot.backing->owner_prev != snapshot.owner_prev ||
            snapshot.backing->references != snapshot.references ||
            snapshot.backing->closing != snapshot.closing)
            return false;
        ++index;
    }
    return true;
}

static void whitebox_restore_chain(
    const std::vector<WhiteboxSnapshot> &snapshots)
{
    for (std::size_t index = 0U; index < snapshots.size(); ++index) {
        const WhiteboxSnapshot &snapshot = snapshots[index];
        snapshot.backing->hash_next = snapshot.hash_next;
        snapshot.backing->identity_next = snapshot.identity_next;
        snapshot.backing->owner_next = snapshot.owner_next;
        snapshot.backing->owner_prev = snapshot.owner_prev;
        snapshot.backing->references = snapshot.references;
        snapshot.backing->closing = snapshot.closing;
    }
}

enum class DestroyCorruption
{
    OwnerCycle,
    CountSmall,
    CountLarge,
    CountHugeCycle,
    HeadPrevious,
    CrossRegistration,
    BaseHashCycle,
    IdentityHashCycle
};

static bool whitebox_destroy_rejects(DestroyCorruption corruption)
{
    MEMPOOL *pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    MEMPOOL *foreign_pool = static_cast<MEMPOOL *>(0);
    WhiteboxRegistration *registration = static_cast<WhiteboxRegistration *>(0);
    WhiteboxRegistration *foreign_registration =
        static_cast<WhiteboxRegistration *>(0);
    WhiteboxBacking *foreign_head = static_cast<WhiteboxBacking *>(0);
    WhiteboxBacking *foreign_previous = static_cast<WhiteboxBacking *>(0);
    std::vector<void *> allocations;
    std::vector<WhiteboxBacking *> nodes;
    std::vector<WhiteboxSnapshot> original_snapshots;
    std::vector<WhiteboxSnapshot> snapshots;
    ULONG original_count = 0UL;
    ULONG index = 0UL;
    ULONG released = 1UL;
    LONG breakpoints_before = 0;
    bool unchanged = false;

    if (pool == static_cast<MEMPOOL *>(0))
        return false;
    while (index < 3UL) {
        void *allocation = Mempool_Alloc(pool, 50000UL);
        if (allocation == static_cast<void *>(0))
            return false;
        static_cast<unsigned char *>(allocation)[0] =
            static_cast<unsigned char>(0x40U + index);
        allocations.push_back(allocation);
        ++index;
    }
    registration = whitebox_registration(pool);
    original_count = registration->backing_count;
    WhiteboxBacking *node = registration->backings;
    index = 0UL;
    while (node != static_cast<WhiteboxBacking *>(0) &&
           index < original_count) {
        nodes.push_back(node);
        node = node->owner_next;
        ++index;
    }
    if (nodes.size() < 4U)
        return false;
    original_snapshots = whitebox_snapshot_chain(registration, original_count);

    switch (corruption) {
    case DestroyCorruption::OwnerCycle:
        nodes[2]->owner_next = nodes[1];
        nodes[1]->owner_prev = nodes[2];
        break;
    case DestroyCorruption::CountSmall:
        registration->backing_count = original_count - 1UL;
        break;
    case DestroyCorruption::CountLarge:
        registration->backing_count = original_count + 1UL;
        break;
    case DestroyCorruption::CountHugeCycle:
        registration->backing_count = 0xFFFFFFFFUL;
        nodes[2]->owner_next = nodes[1];
        nodes[1]->owner_prev = nodes[2];
        break;
    case DestroyCorruption::HeadPrevious:
        nodes[0]->owner_prev = nodes[1];
        break;
    case DestroyCorruption::CrossRegistration:
        foreign_pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
        if (foreign_pool == static_cast<MEMPOOL *>(0))
            return false;
        if (Mempool_Alloc(foreign_pool, 50000UL) == static_cast<void *>(0))
            return false;
        foreign_registration = whitebox_registration(foreign_pool);
        foreign_head = foreign_registration->backings;
        foreign_previous = foreign_head->owner_prev;
        nodes[1]->owner_next = foreign_head;
        foreign_head->owner_prev = nodes[1];
        break;
    case DestroyCorruption::BaseHashCycle:
        nodes[0]->hash_next = nodes[0];
        break;
    case DestroyCorruption::IdentityHashCycle:
        nodes[0]->identity_next = nodes[0];
        break;
    }

    snapshots = whitebox_snapshot_chain(registration, original_count);
    breakpoints_before = InterlockedCompareExchange(&whitebox_breakpoints, 0, 0);
    {
        BreakpointScope breakpoint_scope;
        if (breakpoint_scope.handle == static_cast<PVOID>(0))
            return false;
        released = Mempool_DestroyPool(pool);
    }
    unchanged = whitebox_chain_unchanged(snapshots) &&
                registration->backing_count ==
                    (corruption == DestroyCorruption::CountSmall ?
                         original_count - 1UL :
                     corruption == DestroyCorruption::CountLarge ?
                         original_count + 1UL :
                     corruption == DestroyCorruption::CountHugeCycle ?
                         0xFFFFFFFFUL : original_count) &&
                InterlockedCompareExchange(&whitebox_breakpoints, 0, 0) >
                    breakpoints_before;

    /* The failed pool is intentionally quarantined/leaked.  Restore only the
       damaged links so later tests do not inherit a corrupt global index. */
    whitebox_restore_chain(original_snapshots);
    registration->backing_count = original_count;
    if (foreign_head != static_cast<WhiteboxBacking *>(0)) {
        foreign_head->owner_prev = foreign_previous;
        (void)Mempool_DestroyPool(foreign_pool);
    }
    return released == 0UL && unchanged &&
           static_cast<unsigned char *>(allocations[0])[0] == 0x40U;
}

enum class FreeCorruption
{
    HeadPrevious,
    BaseHashCycle,
    IdentityHashCycle
};

static bool whitebox_free_retire_rejects(FreeCorruption corruption)
{
    MEMPOOL *pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    void *first = static_cast<void *>(0);
    void *target_memory = static_cast<void *>(0);
    WhiteboxRegistration *registration = static_cast<WhiteboxRegistration *>(0);
    WhiteboxBacking *target = static_cast<WhiteboxBacking *>(0);
    WhiteboxBacking *neighbor = static_cast<WhiteboxBacking *>(0);
    std::vector<WhiteboxSnapshot> original_snapshots;
    std::vector<WhiteboxSnapshot> snapshots;
    ULONG original_count = 0UL;
    LONG breakpoints_before = 0;
    bool rejected = false;

    if (pool == static_cast<MEMPOOL *>(0))
        return false;
    first = Mempool_Alloc(pool, 50000UL);
    target_memory = Mempool_Alloc(pool, 50000UL);
    if (first == static_cast<void *>(0) ||
        target_memory == static_cast<void *>(0))
        return false;
    static_cast<unsigned char *>(target_memory)[0] = 0xA6U;
    registration = whitebox_registration(pool);
    original_count = registration->backing_count;
    target = whitebox_find_backing(pool, target_memory);
    if (target == static_cast<WhiteboxBacking *>(0) ||
        registration->backings != target)
        return false;
    original_snapshots = whitebox_snapshot_chain(registration, original_count);

    switch (corruption) {
    case FreeCorruption::HeadPrevious:
        neighbor = target->owner_next;
        if (neighbor == static_cast<WhiteboxBacking *>(0))
            return false;
        target->owner_prev = neighbor;
        neighbor->owner_next = target;
        break;
    case FreeCorruption::BaseHashCycle:
        target->hash_next = target;
        break;
    case FreeCorruption::IdentityHashCycle:
        target->identity_next = target;
        break;
    }

    snapshots = whitebox_snapshot_chain(registration, original_count);
    breakpoints_before = InterlockedCompareExchange(&whitebox_breakpoints, 0, 0);
    {
        BreakpointScope breakpoint_scope;
        if (breakpoint_scope.handle == static_cast<PVOID>(0))
            return false;
        Mempool_Free(target_memory);
    }

    rejected = InterlockedCompareExchange(&whitebox_breakpoints, 0, 0) >
                   breakpoints_before &&
               registration->backing_count == original_count &&
               whitebox_chain_unchanged(snapshots) &&
               static_cast<unsigned char *>(target_memory)[0] == 0xA6U;
    whitebox_restore_chain(original_snapshots);
    Mempool_Free(target_memory);
    Mempool_Free(first);
    return rejected && Mempool_DestroyPool(pool) >= 1UL;
}

enum class PageListCorruption
{
    NextSelf,
    PreviousSelf,
    NextBacklink,
    PreviousBacklink,
    HeadMismatch,
    TailMismatch,
    CountSmall,
    CountLarge,
    PageCountCheck,
    BinCount,
    BinCheck,
    BinHeadMismatch
};

static bool whitebox_page_list_free_rejects(PageListCorruption corruption)
{
    MEMPOOL *pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    WhiteboxPool *private_pool = static_cast<WhiteboxPool *>(0);
    WhiteboxPage *owner_page = static_cast<WhiteboxPage *>(0);
    WhiteboxPage *target_page = static_cast<WhiteboxPage *>(0);
    WhiteboxPage *allocation_page = static_cast<WhiteboxPage *>(0);
    WhiteboxListElement *original_next = static_cast<WhiteboxListElement *>(0);
    WhiteboxListElement *original_previous =
        static_cast<WhiteboxListElement *>(0);
    WhiteboxListElement *next_original_previous =
        static_cast<WhiteboxListElement *>(0);
    WhiteboxListElement *previous_original_next =
        static_cast<WhiteboxListElement *>(0);
    WhiteboxPage *next_page = static_cast<WhiteboxPage *>(0);
    WhiteboxPage *previous_page = static_cast<WhiteboxPage *>(0);
    WhiteboxListElement *original_head = static_cast<WhiteboxListElement *>(0);
    WhiteboxListElement *original_tail = static_cast<WhiteboxListElement *>(0);
    WhiteboxRegistration *registration = static_cast<WhiteboxRegistration *>(0);
    WhiteboxBacking *registration_head = static_cast<WhiteboxBacking *>(0);
    std::vector<void *> allocations;
    std::vector<WhiteboxSnapshot> backing_snapshots;
    std::vector<WhiteboxPageHeaderSnapshot> page_snapshots;
    std::vector<unsigned char> target_page_snapshot;
    WhiteboxPageIndexSnapshot index_snapshot = {};
    void *first_on_target = static_cast<void *>(0);
    void *target_memory = static_cast<void *>(0);
    std::size_t first_on_target_index = 0U;
    std::size_t target_index = 0U;
    std::size_t index = 0U;
    ULONG registration_count = 0UL;
    ULONG original_page_count = 0UL;
    ULONG original_page_count_check = 0UL;
    USHORT source_run = 0;
    USHORT original_bin_count = 0;
    USHORT original_bin_check = 0;
    WhiteboxPage *original_bin_head = static_cast<WhiteboxPage *>(0);
    int original_list_count = 0;
    LONG breakpoints_before = 0;
    bool rejected = false;

    if (pool == static_cast<MEMPOOL *>(0))
        return false;
    private_pool = whitebox_pool(pool);
    owner_page = whitebox_page_from_pointer(pool);
    while (index < 12U && target_memory == static_cast<void *>(0)) {
        void *memory = Mempool_Alloc(pool, 24000UL);

        if (memory == static_cast<void *>(0))
            return false;
        allocations.push_back(memory);
        allocation_page = whitebox_page_from_pointer(memory);
        if (allocation_page != owner_page) {
            if (target_page == static_cast<WhiteboxPage *>(0)) {
                target_page = allocation_page;
                first_on_target = memory;
                first_on_target_index = allocations.size() - 1U;
            } else if (allocation_page == target_page) {
                target_memory = memory;
                target_index = allocations.size() - 1U;
            }
        }
        ++index;
    }
    if (target_memory == static_cast<void *>(0) ||
        first_on_target == static_cast<void *>(0))
        return false;
    if (corruption == PageListCorruption::PreviousBacklink) {
        void *later_first = Mempool_Alloc(pool, 24000UL);
        void *later_second = Mempool_Alloc(pool, 24000UL);

        if (later_first == static_cast<void *>(0) ||
            later_second == static_cast<void *>(0) ||
            whitebox_page_from_pointer(later_first) == target_page ||
            whitebox_page_from_pointer(later_second) !=
                whitebox_page_from_pointer(later_first))
            return false;
        allocations.push_back(later_first);
        allocations.push_back(later_second);
    }
    Mempool_Free(first_on_target);
    allocations[first_on_target_index] = static_cast<void *>(0);
    static_cast<unsigned char *>(target_memory)[0] = 0xB7U;

    registration = whitebox_registration(pool);
    registration_count = registration->backing_count;
    registration_head = registration->backings;
    original_next = target_page->list_elem.next;
    original_previous = target_page->list_elem.prev;
    original_head = private_pool->pages.head;
    original_tail = private_pool->pages.tail;
    original_list_count = private_pool->pages.count;
    original_page_count = private_pool->page_count;
    original_page_count_check = private_pool->page_count_check;
    source_run = target_page->max_free_run;
    original_bin_count = private_pool->run_bin_counts[source_run];
    original_bin_check = private_pool->run_bin_checks[source_run];
    original_bin_head = private_pool->run_bins[source_run];
    if (original_next != static_cast<WhiteboxListElement *>(0)) {
        next_page = reinterpret_cast<WhiteboxPage *>(original_next);
        next_original_previous = next_page->list_elem.prev;
    }
    if (original_previous != static_cast<WhiteboxListElement *>(0)) {
        previous_page = reinterpret_cast<WhiteboxPage *>(original_previous);
        previous_original_next = previous_page->list_elem.next;
    }

    switch (corruption) {
    case PageListCorruption::NextSelf:
        target_page->list_elem.next = &target_page->list_elem;
        break;
    case PageListCorruption::PreviousSelf:
        target_page->list_elem.prev = &target_page->list_elem;
        break;
    case PageListCorruption::NextBacklink:
        if (next_page == static_cast<WhiteboxPage *>(0))
            return false;
        next_page->list_elem.prev = static_cast<WhiteboxListElement *>(0);
        break;
    case PageListCorruption::PreviousBacklink:
        if (previous_page == static_cast<WhiteboxPage *>(0))
            return false;
        previous_page->list_elem.next = static_cast<WhiteboxListElement *>(0);
        break;
    case PageListCorruption::HeadMismatch:
        if (original_next == static_cast<WhiteboxListElement *>(0))
            return false;
        private_pool->pages.head = original_next;
        break;
    case PageListCorruption::TailMismatch:
        private_pool->pages.tail = &target_page->list_elem;
        break;
    case PageListCorruption::CountSmall:
        --private_pool->pages.count;
        break;
    case PageListCorruption::CountLarge:
        ++private_pool->pages.count;
        break;
    case PageListCorruption::PageCountCheck:
        ++private_pool->page_count_check;
        break;
    case PageListCorruption::BinCount:
        ++private_pool->run_bin_counts[source_run];
        break;
    case PageListCorruption::BinCheck:
        ++private_pool->run_bin_checks[source_run];
        break;
    case PageListCorruption::BinHeadMismatch:
        private_pool->run_bins[source_run] = owner_page;
        break;
    }

    /* Keep the page resident even if a regression retires it, allowing this
       test to inspect the complete failed transaction without a test UAF. */
    (void)InterlockedIncrement(&target_page->backing.references);
    index_snapshot = whitebox_snapshot_page_index(pool);
    page_snapshots = whitebox_snapshot_page_headers(registration);
    target_page_snapshot.resize(WHITEBOX_PAGE_SIZE);
    std::memcpy(target_page_snapshot.data(), target_page,
                WHITEBOX_PAGE_SIZE);
    backing_snapshots = whitebox_snapshot_chain(registration,
                                                 registration_count);
    breakpoints_before = InterlockedCompareExchange(&whitebox_breakpoints,
                                                     0, 0);
    {
        BreakpointScope breakpoint_scope;
        if (breakpoint_scope.handle == static_cast<PVOID>(0))
            return false;
        Mempool_Free(target_memory);
    }
    rejected = InterlockedCompareExchange(&whitebox_breakpoints, 0, 0) >
                   breakpoints_before &&
               registration->backing_count == registration_count &&
               registration->backings == registration_head &&
               whitebox_chain_unchanged(backing_snapshots) &&
               whitebox_page_headers_unchanged(page_snapshots) &&
               whitebox_page_index_unchanged(pool, index_snapshot) &&
               std::memcmp(target_page, target_page_snapshot.data(),
                           WHITEBOX_PAGE_SIZE) == 0 &&
               static_cast<unsigned char *>(target_memory)[0] == 0xB7U;
    if (!rejected)
        return false;

    (void)InterlockedDecrement(&target_page->backing.references);
    target_page->list_elem.next = original_next;
    target_page->list_elem.prev = original_previous;
    if (next_page != static_cast<WhiteboxPage *>(0))
        next_page->list_elem.prev = next_original_previous;
    if (previous_page != static_cast<WhiteboxPage *>(0))
        previous_page->list_elem.next = previous_original_next;
    private_pool->pages.head = original_head;
    private_pool->pages.tail = original_tail;
    private_pool->pages.count = original_list_count;
    private_pool->page_count = original_page_count;
    private_pool->page_count_check = original_page_count_check;
    private_pool->run_bin_counts[source_run] = original_bin_count;
    private_pool->run_bin_checks[source_run] = original_bin_check;
    private_pool->run_bins[source_run] = original_bin_head;
    Mempool_Free(target_memory);
    allocations[target_index] = static_cast<void *>(0);
    for (index = 0U; index < allocations.size(); ++index) {
        if (allocations[index] != static_cast<void *>(0))
            Mempool_Free(allocations[index]);
    }
    return Mempool_DestroyPool(pool) >= 1UL;
}

enum class LargeListInsertCorruption
{
    HeadPrevious,
    TailNext,
    HeadMismatch,
    ListCount,
    CountCheck
};

static bool whitebox_large_insert_rejects(
    LargeListInsertCorruption corruption)
{
    MEMPOOL *pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    WhiteboxPool *private_pool = static_cast<WhiteboxPool *>(0);
    WhiteboxRegistration *registration = static_cast<WhiteboxRegistration *>(0);
    WhiteboxLargeChunk *head = static_cast<WhiteboxLargeChunk *>(0);
    WhiteboxLargeChunk *tail = static_cast<WhiteboxLargeChunk *>(0);
    WhiteboxList original_list = {};
    WhiteboxList corrupt_list = {};
    WhiteboxListElement corrupt_head_element = {};
    WhiteboxListElement corrupt_tail_element = {};
    WhiteboxListElement *head_previous = static_cast<WhiteboxListElement *>(0);
    WhiteboxListElement *tail_next = static_cast<WhiteboxListElement *>(0);
    std::vector<WhiteboxSnapshot> backing_snapshots;
    void *first = static_cast<void *>(0);
    void *second = static_cast<void *>(0);
    void *unexpected = static_cast<void *>(0);
    ULONG original_count = 0UL;
    ULONG original_count_check = 0UL;
    ULONG registration_count = 0UL;
    LONG breakpoints_before = 0;
    bool rejected = false;

    if (pool == static_cast<MEMPOOL *>(0))
        return false;
    first = Mempool_Alloc(pool, 50000UL);
    second = Mempool_Alloc(pool, 50000UL);
    if (first == static_cast<void *>(0) || second == static_cast<void *>(0))
        return false;
    static_cast<unsigned char *>(first)[0] = 0x51U;
    static_cast<unsigned char *>(second)[0] = 0xA2U;
    private_pool = whitebox_pool(pool);
    registration = whitebox_registration(pool);
    original_list = private_pool->large_chunks;
    original_count = private_pool->large_chunk_count;
    original_count_check = private_pool->large_chunk_count_check;
    head = reinterpret_cast<WhiteboxLargeChunk *>(original_list.head);
    tail = reinterpret_cast<WhiteboxLargeChunk *>(original_list.tail);
    if (head == static_cast<WhiteboxLargeChunk *>(0) ||
        tail == static_cast<WhiteboxLargeChunk *>(0) || head == tail)
        return false;
    head_previous = head->list_elem.prev;
    tail_next = tail->list_elem.next;

    switch (corruption) {
    case LargeListInsertCorruption::HeadPrevious:
        head->list_elem.prev = &tail->list_elem;
        break;
    case LargeListInsertCorruption::TailNext:
        tail->list_elem.next = &head->list_elem;
        break;
    case LargeListInsertCorruption::HeadMismatch:
        private_pool->large_chunks.head = &tail->list_elem;
        break;
    case LargeListInsertCorruption::ListCount:
        ++private_pool->large_chunks.count;
        break;
    case LargeListInsertCorruption::CountCheck:
        ++private_pool->large_chunk_count_check;
        break;
    }
    corrupt_list = private_pool->large_chunks;
    corrupt_head_element = head->list_elem;
    corrupt_tail_element = tail->list_elem;
    registration_count = registration->backing_count;
    backing_snapshots = whitebox_snapshot_chain(registration,
                                                 registration_count);
    breakpoints_before = InterlockedCompareExchange(&whitebox_breakpoints,
                                                     0, 0);
    {
        BreakpointScope breakpoint_scope;
        if (breakpoint_scope.handle == static_cast<PVOID>(0))
            return false;
        unexpected = Mempool_Alloc(pool, 50000UL);
    }
    rejected = unexpected == static_cast<void *>(0) &&
               InterlockedCompareExchange(&whitebox_breakpoints, 0, 0) >
                   breakpoints_before &&
               std::memcmp(&private_pool->large_chunks, &corrupt_list,
                           sizeof(corrupt_list)) == 0 &&
               std::memcmp(&head->list_elem, &corrupt_head_element,
                           sizeof(corrupt_head_element)) == 0 &&
               std::memcmp(&tail->list_elem, &corrupt_tail_element,
                           sizeof(corrupt_tail_element)) == 0 &&
               private_pool->large_chunk_count == original_count &&
               private_pool->large_chunk_count_check ==
                   (corruption == LargeListInsertCorruption::CountCheck ?
                        original_count_check + 1UL : original_count_check) &&
               registration->backing_count == registration_count &&
               whitebox_chain_unchanged(backing_snapshots) &&
               static_cast<unsigned char *>(first)[0] == 0x51U &&
               static_cast<unsigned char *>(second)[0] == 0xA2U;
    if (!rejected)
        return false;

    private_pool->large_chunks = original_list;
    private_pool->large_chunk_count = original_count;
    private_pool->large_chunk_count_check = original_count_check;
    head->list_elem.prev = head_previous;
    tail->list_elem.next = tail_next;
    Mempool_Free(second);
    Mempool_Free(first);
    return Mempool_DestroyPool(pool) >= 1UL;
}

static bool whitebox_publish_rejects_corrupt_base_bucket()
{
    MEMPOOL *pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    WhiteboxRegistration *registration = static_cast<WhiteboxRegistration *>(0);
    WhiteboxBacking *bucket_nodes[256] = {};
    std::vector<void *> allocations;
    std::vector<WhiteboxSnapshot> original_snapshots;
    std::vector<WhiteboxSnapshot> corrupt_snapshots;
    ULONG covered = 0UL;
    ULONG index = 0UL;
    ULONG original_count = 0UL;
    void *unexpected = static_cast<void *>(0);
    bool unchanged = false;
    ULONG released = 0UL;

    if (pool == static_cast<MEMPOOL *>(0))
        return false;
    registration = whitebox_registration(pool);
    while (covered < 256UL && index < 1024UL) {
        void *allocation = Mempool_Alloc(pool, 50000UL);
        WhiteboxBacking *backing = static_cast<WhiteboxBacking *>(0);
        ULONG bucket = 0UL;

        if (allocation == static_cast<void *>(0)) {
            (void)Mempool_DestroyPool(pool);
            return false;
        }
        allocations.push_back(allocation);
        backing = whitebox_find_backing(pool, allocation);
        if (backing == static_cast<WhiteboxBacking *>(0)) {
            (void)Mempool_DestroyPool(pool);
            return false;
        }
        bucket = whitebox_pointer_bucket(backing->base);
        if (bucket_nodes[bucket] == static_cast<WhiteboxBacking *>(0)) {
            bucket_nodes[bucket] = backing;
            ++covered;
        }
        ++index;
    }
    if (covered != 256UL) {
        (void)Mempool_DestroyPool(pool);
        return false;
    }

    original_count = registration->backing_count;
    original_snapshots = whitebox_snapshot_chain(registration, original_count);
    index = 0UL;
    while (index < 256UL) {
        bucket_nodes[index]->hash_next = bucket_nodes[index];
        ++index;
    }
    corrupt_snapshots = whitebox_snapshot_chain(registration, original_count);

    /* Every possible target base bucket is now cyclic.  Publication must
       distinguish that corruption from an ordinary not-found result and roll
       the raw backing/descriptor allocation back without touching indexes. */
    {
        BreakpointScope breakpoint_scope;
        if (breakpoint_scope.handle == static_cast<PVOID>(0))
            return false;
        unexpected = Mempool_Alloc(pool, 50000UL);
    }
    unchanged = unexpected == static_cast<void *>(0) &&
                registration->backing_count == original_count &&
                whitebox_chain_unchanged(corrupt_snapshots);

    whitebox_restore_chain(original_snapshots);
    if (unexpected != static_cast<void *>(0))
        Mempool_Free(unexpected);
    released = Mempool_DestroyPool(pool);
    return unchanged && released >= 1UL;
}

static bool whitebox_registration_cycle_rejected()
{
    MEMPOOL *pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    void *allocation = static_cast<void *>(0);
    WhiteboxRegistration *registration = static_cast<WhiteboxRegistration *>(0);
    WhiteboxRegistration *original_next = static_cast<WhiteboxRegistration *>(0);
    std::vector<WhiteboxSnapshot> snapshots;
    ULONG backing_count = 0UL;
    LONG lifecycle = 0;
    LONG breakpoints_before = 0;
    ULONG released = 1UL;
    bool rejected = false;

    if (pool == static_cast<MEMPOOL *>(0))
        return false;
    allocation = Mempool_Alloc(pool, 50000UL);
    if (allocation == static_cast<void *>(0))
        return false;
    registration = whitebox_registration(pool);
    original_next = registration->next;
    backing_count = registration->backing_count;
    lifecycle = registration->lifecycle_state;
    snapshots = whitebox_snapshot_chain(registration, backing_count);
    registration->next = registration;

    if (Mempool_Alloc(pool, 32UL) != static_cast<void *>(0))
        return false;
    breakpoints_before = InterlockedCompareExchange(&whitebox_breakpoints, 0, 0);
    {
        BreakpointScope breakpoint_scope;
        if (breakpoint_scope.handle == static_cast<PVOID>(0))
            return false;
        Mempool_Free(allocation);
        released = Mempool_DestroyPool(pool);
    }
    rejected = released == 0UL &&
               registration->next == registration &&
               registration->lifecycle_state == lifecycle &&
               registration->backing_count == backing_count &&
               whitebox_chain_unchanged(snapshots) &&
               InterlockedCompareExchange(&whitebox_breakpoints, 0, 0) >=
                   breakpoints_before + 2;

    registration->next = original_next;
    Mempool_Free(allocation);
    return rejected && Mempool_DestroyPool(pool) >= 1UL;
}

struct WhiteboxRegistrationLink
{
    WhiteboxRegistration *registration;
    WhiteboxRegistration *next;
    WhiteboxBacking *backings;
    ULONG backing_count;
    LONG lifecycle_state;
};

static bool whitebox_create_rejects_corrupt_registration_bucket()
{
    WhiteboxRegistration *bucket_nodes[256] = {};
    std::vector<MEMPOOL *> pools;
    std::vector<WhiteboxRegistrationLink> links;
    MEMPOOL *unexpected = static_cast<MEMPOOL *>(0);
    ULONG covered = 0UL;
    ULONG attempts = 0UL;
    bool unchanged = true;

    while (covered < 256UL && attempts < 4096UL) {
        MEMPOOL *pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
        WhiteboxRegistration *registration =
            static_cast<WhiteboxRegistration *>(0);
        ULONG bucket = 0UL;

        if (pool == static_cast<MEMPOOL *>(0))
            break;
        pools.push_back(pool);
        registration = whitebox_registration(pool);
        bucket = whitebox_pointer_bucket(pool);
        if (bucket_nodes[bucket] == static_cast<WhiteboxRegistration *>(0)) {
            bucket_nodes[bucket] = registration;
            ++covered;
        }
        ++attempts;
    }
    if (covered != 256UL) {
        while (!pools.empty()) {
            (void)Mempool_DestroyPool(pools.back());
            pools.pop_back();
        }
        return false;
    }

    links.reserve(256U);
    for (ULONG bucket = 0UL; bucket < 256UL; ++bucket) {
        WhiteboxRegistrationLink link = {};
        link.registration = bucket_nodes[bucket];
        link.next = bucket_nodes[bucket]->next;
        link.backings = bucket_nodes[bucket]->backings;
        link.backing_count = bucket_nodes[bucket]->backing_count;
        link.lifecycle_state = bucket_nodes[bucket]->lifecycle_state;
        links.push_back(link);
        bucket_nodes[bucket]->next = bucket_nodes[bucket];
    }

    unexpected = Mempool_CreatePool(MEMPOOL_NONPAGED);
    if (unexpected != static_cast<MEMPOOL *>(0))
        unchanged = false;
    for (std::size_t index = 0U; index < links.size(); ++index) {
        const WhiteboxRegistrationLink &link = links[index];
        if (link.registration->next != link.registration ||
            link.registration->backings != link.backings ||
            link.registration->backing_count != link.backing_count ||
            link.registration->lifecycle_state != link.lifecycle_state)
            unchanged = false;
    }

    for (std::size_t index = 0U; index < links.size(); ++index)
        links[index].registration->next = links[index].next;
    if (unexpected != static_cast<MEMPOOL *>(0))
        (void)Mempool_DestroyPool(unexpected);
    while (!pools.empty()) {
        if (Mempool_DestroyPool(pools.back()) == 0UL)
            unchanged = false;
        pools.pop_back();
    }
    return unchanged;
}

#if defined(MEMPOOL_TESTING)
static bool whitebox_registration_count_rejected(
    MEMPOOL_REGISTRY_TEST_CORRUPTION corruption)
{
    MEMPOOL *pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    void *allocation = static_cast<void *>(0);
    MEMPOOL_REGISTRY_TEST_SNAPSHOT registry_snapshot = {};
    WhiteboxRegistration *registration = static_cast<WhiteboxRegistration *>(0);
    std::vector<WhiteboxSnapshot> backing_snapshots;
    LONG lifecycle = 0;
    LONG breakpoints_before = 0;
    ULONG released = 1UL;
    bool rejected = false;

    if (pool == static_cast<MEMPOOL *>(0))
        return false;
    allocation = Mempool_Alloc(pool, 50000UL);
    if (allocation == static_cast<void *>(0) ||
        !mempool_test_registry_snapshot(pool, &registry_snapshot))
        return false;
    registration = whitebox_registration(pool);
    lifecycle = registration->lifecycle_state;
    backing_snapshots = whitebox_snapshot_chain(
                            registration, registration->backing_count);
    if (!mempool_test_registry_corrupt(&registry_snapshot, corruption))
        return false;

    if (Mempool_Alloc(pool, 32UL) != static_cast<void *>(0))
        return false;
    if (Mempool_CreatePool(MEMPOOL_NONPAGED) != static_cast<MEMPOOL *>(0))
        return false;
    breakpoints_before = InterlockedCompareExchange(&whitebox_breakpoints, 0, 0);
    {
        BreakpointScope breakpoint_scope;
        if (breakpoint_scope.handle == static_cast<PVOID>(0))
            return false;
        Mempool_Free(allocation);
        released = Mempool_DestroyPool(pool);
    }
    rejected = released == 0UL &&
               registration->lifecycle_state == lifecycle &&
               whitebox_chain_unchanged(backing_snapshots) &&
               InterlockedCompareExchange(&whitebox_breakpoints, 0, 0) >=
                   breakpoints_before + 2;

    mempool_test_registry_restore(&registry_snapshot);
    Mempool_Free(allocation);
    return rejected && Mempool_DestroyPool(pool) >= 1UL;
}
#endif

static MEMPOOL_TYPE invalid_pool_type()
{
    MEMPOOL_TYPE value = MEMPOOL_PAGED;
    unsigned int raw = 99U;

    /* Form an invalid enum value without triggering -Wconversion in GCC. */
    static_assert(sizeof(value) == sizeof(raw),
                  "MEMPOOL_TYPE must have the Windows enum width");
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

struct PoolOwner
{
    MEMPOOL *pool;

    PoolOwner() : pool(static_cast<MEMPOOL *>(0))
    {
        pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    }

    ~PoolOwner()
    {
        if (pool != static_cast<MEMPOOL *>(0)) {
            (void)Mempool_DestroyPool(pool);
            pool = static_cast<MEMPOOL *>(0);
        }
    }

    PoolOwner(const PoolOwner &) = delete;
    PoolOwner &operator=(const PoolOwner &) = delete;
};

static void exercise_pool(MEMPOOL *pool, unsigned int seed)
{
    unsigned int state = seed;
    unsigned int index = 0U;
    ULONG size = 0UL;
    void *memory = static_cast<void *>(0);

    while (index < 300U) {
        state = state * 1664525U + 1013904223U;
        size = static_cast<ULONG>(1UL + (state % 2048U));
        memory = Mempool_Alloc(pool, size);
        if (memory != static_cast<void *>(0)) {
            static_cast<unsigned char *>(memory)[0] = 0x5AU;
            static_cast<unsigned char *>(memory)[size - 1UL] = 0xA5U;
            Mempool_Free(memory);
        }
        ++index;
    }
}

static void churn_independent_pools()
{
    unsigned int index = 0U;

    while (index < 100U) {
        MEMPOOL *pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
        if (pool != static_cast<MEMPOOL *>(0)) {
            void *memory = Mempool_Alloc(pool, 48UL);
            if (memory != static_cast<void *>(0))
                Mempool_Free(memory);
            (void)Mempool_DestroyPool(pool);
        }
        ++index;
    }
}

} /* namespace */

TEST_CASE("mempool rejects invalid handles and zero-size requests",
          "[mempool][validation]")
{
    MEMPOOL *pool = static_cast<MEMPOOL *>(0);
    void *memory = static_cast<void *>(0);
    ULONG released = 0UL;

    REQUIRE(Mempool_CreatePool(invalid_pool_type()) ==
            static_cast<MEMPOOL *>(0));
    REQUIRE(Mempool_Alloc(static_cast<MEMPOOL *>(0), 1UL) ==
            static_cast<void *>(0));
    REQUIRE(Mempool_Alloc(static_cast<MEMPOOL *>(0), 0UL) ==
            static_cast<void *>(0));

    pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    REQUIRE(pool != static_cast<MEMPOOL *>(0));
    memory = Mempool_Alloc(pool, 0UL);
    REQUIRE(memory == static_cast<void *>(0));
    released = Mempool_DestroyPool(pool);
    REQUIRE(released >= 1UL);
    /* A just-destroyed handle is absent from the live registry.  As with any
       raw-pointer handle, callers must not retain it across address reuse. */
    REQUIRE(Mempool_DestroyPool(pool) == 0UL);
}

TEST_CASE("mempool serves aligned small allocations and preserves data",
          "[mempool][small]")
{
    PoolOwner owner = PoolOwner();
    std::vector<void *> allocations;
    const ULONG sizes[] = {1UL, 7UL, 16UL, 127UL, 128UL, 129UL, 1024UL};
    const std::size_t size_count = sizeof(sizes) / sizeof(sizes[0]);
    std::size_t index = 0U;
    void *memory = static_cast<void *>(0);
    unsigned char *bytes = static_cast<unsigned char *>(0);
    std::uintptr_t address = static_cast<std::uintptr_t>(0U);

    REQUIRE(owner.pool != static_cast<MEMPOOL *>(0));
    allocations.reserve(size_count);
    while (index < size_count) {
        memory = Mempool_Alloc(owner.pool, sizes[index]);
        REQUIRE(memory != static_cast<void *>(0));
        address = reinterpret_cast<std::uintptr_t>(memory);
        const std::uintptr_t required_alignment =
            sizeof(void *) == 8U ? static_cast<std::uintptr_t>(16U) :
                                  static_cast<std::uintptr_t>(8U);
        REQUIRE((address % required_alignment) ==
                static_cast<std::uintptr_t>(0U));
        bytes = static_cast<unsigned char *>(memory);
        bytes[0] = 0x11U;
        bytes[sizes[index] - 1UL] = 0xEEU;
        if (sizes[index] == 1UL)
            REQUIRE(bytes[0] == 0xEEU);
        else
            REQUIRE(bytes[0] == 0x11U);
        REQUIRE(bytes[sizes[index] - 1UL] == 0xEEU);
        allocations.push_back(memory);
        ++index;
    }
    while (!allocations.empty()) {
        Mempool_Free(allocations.back());
        allocations.pop_back();
    }
}

TEST_CASE("mempool preserves adjacent allocations at size boundaries",
          "[mempool][metadata][boundary]")
{
    PoolOwner owner = PoolOwner();
    const ULONG sizes[] = {1UL, 15UL, 16UL, 17UL, 111UL, 112UL,
                           113UL, 49135UL, 49136UL, 49137UL};
    const std::size_t count = sizeof(sizes) / sizeof(sizes[0]);
    std::size_t index = 0U;
    void *first = static_cast<void *>(0);
    void *second = static_cast<void *>(0);
    void *replacement = static_cast<void *>(0);

    REQUIRE(owner.pool != static_cast<MEMPOOL *>(0));
    while (index < count) {
        first = Mempool_Alloc(owner.pool, sizes[index]);
        second = Mempool_Alloc(owner.pool, 37UL);
        REQUIRE(first != static_cast<void *>(0));
        REQUIRE(second != static_cast<void *>(0));
        std::memset(first, 0x31, static_cast<std::size_t>(sizes[index]));
        std::memset(second, 0xA7, 37U);
        Mempool_Free(first);
        replacement = Mempool_Alloc(owner.pool, sizes[index]);
        REQUIRE(replacement != static_cast<void *>(0));
        REQUIRE(static_cast<unsigned char *>(second)[0] == 0xA7U);
        REQUIRE(static_cast<unsigned char *>(second)[36] == 0xA7U);
        Mempool_Free(replacement);
        Mempool_Free(second);
        ++index;
    }
}

TEST_CASE("mempool handles page-sized allocations and overflow limits",
          "[mempool][large]")
{
    PoolOwner owner = PoolOwner();
    void *large = static_cast<void *>(0);
    void *too_large = static_cast<void *>(0);
    unsigned char *bytes = static_cast<unsigned char *>(0);

    REQUIRE(owner.pool != static_cast<MEMPOOL *>(0));
    large = Mempool_Alloc(owner.pool, 65536UL);
    REQUIRE(large != static_cast<void *>(0));
    bytes = static_cast<unsigned char *>(large);
    bytes[0] = 0x3CU;
    bytes[65535U] = 0xC3U;
    REQUIRE(bytes[0] == 0x3CU);
    REQUIRE(bytes[65535U] == 0xC3U);
    Mempool_Free(large);

    too_large = Mempool_Alloc(owner.pool, 0xFFFFFFFFUL);
    REQUIRE(too_large == static_cast<void *>(0));
}

#if defined(MEMPOOL_TESTING)
TEST_CASE("DPC zeroing limit uses exact small and large rounded capacities",
          "[mempool][zero][dpc][boundary]")
{
    const unsigned long long limit =
        static_cast<unsigned long long>(MEMPOOL_MAX_DPC_ZERO_BYTES);
    const bool cap_disabled = limit == 0ULL;

    /* In the user-mode test layout the 16-byte hidden header makes 49136 the
       final small request and 49137 the first page-rounded large request. */
    REQUIRE((mempool_test_dpc_zero_request_allowed(49136UL) != 0) ==
            (cap_disabled || 49136ULL <= limit));
    REQUIRE((mempool_test_dpc_zero_request_allowed(49137UL) != 0) ==
            (cap_disabled || 65520ULL <= limit));
    REQUIRE((mempool_test_dpc_zero_request_allowed(65520UL) != 0) ==
            (cap_disabled || 65520ULL <= limit));
    REQUIRE((mempool_test_dpc_zero_request_allowed(65521UL) != 0) ==
            (cap_disabled || 131056ULL <= limit));
    REQUIRE(mempool_test_dpc_zero_request_allowed(0UL) == 0);
    REQUIRE(mempool_test_dpc_zero_request_allowed(0xFFFFFFFFUL) == 0);
}
#endif

TEST_CASE("large allocation tail cannot corrupt allocator metadata",
          "[mempool][large][metadata]")
{
    PoolOwner owner = PoolOwner();
    const ULONG request_size = 49137UL;
    const std::size_t rounded_payload_capacity = 65536U - 16U;
    unsigned char *memory = static_cast<unsigned char *>(0);
    ULONG released = 0UL;

    REQUIRE(owner.pool != static_cast<MEMPOOL *>(0));
    memory = static_cast<unsigned char *>(
        Mempool_Alloc(owner.pool, request_size));
    REQUIRE(memory != static_cast<unsigned char *>(0));
    /* Deliberately scribble the accessible padding through the last byte of
       the backing region.  Older builds stored the descriptor in this tail. */
    std::memset(memory + request_size, 0xD3,
                rounded_payload_capacity - request_size);
    Mempool_Free(memory);

    memory = static_cast<unsigned char *>(
        Mempool_Alloc(owner.pool, request_size));
    REQUIRE(memory != static_cast<unsigned char *>(0));
    std::memset(memory + request_size, 0x6E,
                rounded_payload_capacity - request_size);
    released = Mempool_DestroyPool(owner.pool);
    owner.pool = static_cast<MEMPOOL *>(0);
    REQUIRE(released >= 1UL);
}

TEST_CASE("large publication validates list boundaries before registry insert",
          "[mempool][large][corruption]")
{
    const LargeListInsertCorruption scenarios[] = {
        LargeListInsertCorruption::HeadPrevious,
        LargeListInsertCorruption::TailNext,
        LargeListInsertCorruption::HeadMismatch,
        LargeListInsertCorruption::ListCount,
        LargeListInsertCorruption::CountCheck
    };
    const std::size_t count = sizeof(scenarios) / sizeof(scenarios[0]);

    for (std::size_t index = 0U; index < count; ++index) {
        INFO("large-list insertion corruption scenario index " << index);
        REQUIRE(whitebox_large_insert_rejects(scenarios[index]));
    }
}

TEST_CASE("mempool grows across pages and reclaims empty pages",
          "[mempool][pages]")
{
    PoolOwner owner = PoolOwner();
    std::vector<void *> allocations;
    std::size_t index = 0U;
    const std::size_t allocation_count = 900U;
    void *memory = static_cast<void *>(0);

    REQUIRE(owner.pool != static_cast<MEMPOOL *>(0));
    allocations.reserve(allocation_count);
    while (index < allocation_count) {
        memory = Mempool_Alloc(owner.pool, 64UL);
        REQUIRE(memory != static_cast<void *>(0));
        allocations.push_back(memory);
        ++index;
    }
    index = 0U;
    while (index < allocations.size()) {
        Mempool_Free(allocations[index]);
        ++index;
    }
    allocations.clear();
    /* The owner page is retained, while fully empty data pages are reclaimed. */
    memory = Mempool_Alloc(owner.pool, 64UL);
    REQUIRE(memory != static_cast<void *>(0));
    Mempool_Free(memory);
}

TEST_CASE("empty-page free rejects corrupt ownership links without mutation",
          "[mempool][pages][corruption]")
{
    const PageListCorruption scenarios[] = {
        PageListCorruption::NextSelf,
        PageListCorruption::PreviousSelf,
        PageListCorruption::NextBacklink,
        PageListCorruption::PreviousBacklink,
        PageListCorruption::HeadMismatch,
        PageListCorruption::TailMismatch,
        PageListCorruption::CountSmall,
        PageListCorruption::CountLarge,
        PageListCorruption::PageCountCheck,
        PageListCorruption::BinCount,
        PageListCorruption::BinCheck,
        PageListCorruption::BinHeadMismatch
    };
    const std::size_t count = sizeof(scenarios) / sizeof(scenarios[0]);

    for (std::size_t index = 0U; index < count; ++index) {
        INFO("page-list corruption scenario index " << index);
        REQUIRE(whitebox_page_list_free_rejects(scenarios[index]));
    }
}

TEST_CASE("fragmented long ownership chains do not make allocation traversal unbounded",
          "[mempool][pages][bounded]")
{
    const std::size_t fragmented_page_count = 128U;
    const std::size_t allocation_count = 2U + fragmented_page_count * 2U;
    MEMPOOL *pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    WhiteboxPool *private_pool = static_cast<WhiteboxPool *>(0);
    WhiteboxPage *owner_page = static_cast<WhiteboxPage *>(0);
    WhiteboxPage *page = static_cast<WhiteboxPage *>(0);
    WhiteboxPage *cycle_page = static_cast<WhiteboxPage *>(0);
    WhiteboxListElement *cycle_next = static_cast<WhiteboxListElement *>(0);
    std::vector<void *> allocations;
    std::vector<WhiteboxPage *> existing_pages;
    void *probe = static_cast<void *>(0);
    std::size_t index = 0U;

    REQUIRE(pool != static_cast<MEMPOOL *>(0));
    private_pool = whitebox_pool(pool);
    owner_page = whitebox_page_from_pointer(pool);
    allocations.reserve(allocation_count);
    while (index < allocation_count) {
        void *memory = Mempool_Alloc(pool, 24000UL);

        REQUIRE(memory != static_cast<void *>(0));
        allocations.push_back(memory);
        ++index;
    }
    REQUIRE(private_pool->page_count >= fragmented_page_count + 1U);

    /* Each non-owner page receives two adjacent requests.  Freeing the lower
       one leaves only 188-cell runs, so a 32 KB request needs a new page. */
    index = 2U;
    while (index + 1U < allocations.size()) {
        REQUIRE(whitebox_page_from_pointer(allocations[index]) ==
                whitebox_page_from_pointer(allocations[index + 1U]));
        REQUIRE(whitebox_page_from_pointer(allocations[index]) != owner_page);
        Mempool_Free(allocations[index]);
        allocations[index] = static_cast<void *>(0);
        index += 2U;
    }

    page = reinterpret_cast<WhiteboxPage *>(private_pool->pages.head);
    index = 0U;
    while (page != static_cast<WhiteboxPage *>(0) &&
           index < private_pool->page_count) {
        existing_pages.push_back(page);
        page = reinterpret_cast<WhiteboxPage *>(page->list_elem.next);
        ++index;
    }
    REQUIRE(existing_pages.size() == private_pool->page_count);
    REQUIRE(existing_pages.size() > 100U);
    cycle_page = existing_pages[existing_pages.size() / 2U];
    REQUIRE(cycle_page != existing_pages.front());
    REQUIRE(cycle_page != existing_pages.back());
    cycle_next = cycle_page->list_elem.next;
    cycle_page->list_elem.next = &cycle_page->list_elem;

    /* The deep ownership cycle is intentionally outside the O(1) insertion
       boundary.  Allocation consults bounded run bins, not this long chain. */
    probe = Mempool_Alloc(pool, 32000UL);
    REQUIRE(probe != static_cast<void *>(0));
    REQUIRE(whitebox_page_from_pointer(probe) != owner_page);
    cycle_page->list_elem.next = cycle_next;
    Mempool_Free(probe);

    for (index = 0U; index < allocations.size(); ++index) {
        if (allocations[index] != static_cast<void *>(0))
            Mempool_Free(allocations[index]);
    }
    REQUIRE(Mempool_DestroyPool(pool) >= 1UL);
}

TEST_CASE("same-run allocations leave bin topology unchanged",
          "[mempool][bins][regression]")
{
    const ULONG request = 16368UL; /* 16-byte header + payload = 128 cells. */
    MEMPOOL *pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    WhiteboxPage *page = static_cast<WhiteboxPage *>(0);
    WhiteboxPage *bin_next = static_cast<WhiteboxPage *>(0);
    WhiteboxPage *bin_previous = static_cast<WhiteboxPage *>(0);
    WhiteboxPageIndexSnapshot snapshot = {};
    void *first = static_cast<void *>(0);
    void *middle = static_cast<void *>(0);
    void *last = static_cast<void *>(0);
    void *replacement = static_cast<void *>(0);
    USHORT maximum = 0;

    REQUIRE(pool != static_cast<MEMPOOL *>(0));
    first = Mempool_Alloc(pool, request);
    middle = Mempool_Alloc(pool, request);
    last = Mempool_Alloc(pool, request);
    REQUIRE(first != static_cast<void *>(0));
    REQUIRE(middle != static_cast<void *>(0));
    REQUIRE(last != static_cast<void *>(0));
    page = whitebox_page_from_pointer(first);
    REQUIRE(whitebox_page_from_pointer(middle) == page);
    REQUIRE(whitebox_page_from_pointer(last) == page);

    Mempool_Free(first);
    Mempool_Free(last);
    maximum = page->max_free_run;
    REQUIRE(maximum >= static_cast<USHORT>(128U));
    bin_next = page->bin_next;
    bin_previous = page->bin_prev;
    snapshot = whitebox_snapshot_page_index(pool);

    replacement = Mempool_Alloc(pool, request);
    REQUIRE(replacement == first);
    REQUIRE(page->max_free_run == maximum);
    REQUIRE(page->bin_next == bin_next);
    REQUIRE(page->bin_prev == bin_previous);
    REQUIRE(whitebox_page_index_unchanged(pool, snapshot));

    Mempool_Free(replacement);
    Mempool_Free(middle);
    REQUIRE(Mempool_DestroyPool(pool) >= 1UL);
}

TEST_CASE("bin zero is bounded and skipped when a page is full",
          "[mempool][bins][boundary]")
{
    MEMPOOL *pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    WhiteboxPool *private_pool = static_cast<WhiteboxPool *>(0);
    WhiteboxPage *owner_page = static_cast<WhiteboxPage *>(0);
    std::vector<void *> allocations;
    void *extra = static_cast<void *>(0);
    USHORT initial_free = 0;
    std::size_t index = 0U;

    REQUIRE(pool != static_cast<MEMPOOL *>(0));
    private_pool = whitebox_pool(pool);
    owner_page = whitebox_page_from_pointer(pool);
    initial_free = owner_page->num_free;
    allocations.reserve(static_cast<std::size_t>(initial_free));
    while (index < static_cast<std::size_t>(initial_free)) {
        void *memory = Mempool_Alloc(pool, 1UL);

        REQUIRE(memory != static_cast<void *>(0));
        REQUIRE(whitebox_page_from_pointer(memory) == owner_page);
        allocations.push_back(memory);
        ++index;
    }
    REQUIRE(owner_page->max_free_run == static_cast<USHORT>(0));
    REQUIRE(private_pool->run_bins[0] == owner_page);
    REQUIRE(private_pool->run_bin_counts[0] >= static_cast<USHORT>(1));

    extra = Mempool_Alloc(pool, 1UL);
    REQUIRE(extra != static_cast<void *>(0));
    REQUIRE(whitebox_page_from_pointer(extra) != owner_page);
    Mempool_Free(extra);
    for (index = 0U; index < allocations.size(); ++index)
        Mempool_Free(allocations[index]);
    REQUIRE(Mempool_DestroyPool(pool) >= 1UL);
}

TEST_CASE("mempool remains usable under concurrent allocation and free",
          "[mempool][threading]")
{
    PoolOwner owner = PoolOwner();
    std::thread first = std::thread();
    std::thread second = std::thread();
    std::thread third = std::thread();
    std::thread fourth = std::thread();

    REQUIRE(owner.pool != static_cast<MEMPOOL *>(0));
    first = std::thread(exercise_pool, owner.pool, 1U);
    second = std::thread(exercise_pool, owner.pool, 2U);
    third = std::thread(exercise_pool, owner.pool, 3U);
    fourth = std::thread(exercise_pool, owner.pool, 4U);
    first.join();
    second.join();
    third.join();
    fourth.join();
    REQUIRE(Mempool_Alloc(owner.pool, 32UL) != static_cast<void *>(0));
}

TEST_CASE("independent pool teardown does not block active pool operations",
          "[mempool][threading][lifecycle]")
{
    PoolOwner owner = PoolOwner();
    std::thread allocator = std::thread();
    std::thread teardown = std::thread();

    REQUIRE(owner.pool != static_cast<MEMPOOL *>(0));
    allocator = std::thread(exercise_pool, owner.pool, 0x1234U);
    teardown = std::thread(churn_independent_pools);
    allocator.join();
    teardown.join();
    void *memory = Mempool_Alloc(owner.pool, 64UL);
    REQUIRE(memory != static_cast<void *>(0));
    Mempool_Free(memory);
}

TEST_CASE("free rejects null or forged backing object metadata without AV",
          "[mempool][metadata][whitebox]")
{
    PoolOwner owner = PoolOwner();
    void *memory = static_cast<void *>(0);
    WhiteboxBacking *backing = static_cast<WhiteboxBacking *>(0);
    void *original_object = static_cast<void *>(0);
    LONG original_references = 0;
    LONG breakpoints_before = 0;

    REQUIRE(owner.pool != static_cast<MEMPOOL *>(0));
    memory = Mempool_Alloc(owner.pool, 50000UL);
    REQUIRE(memory != static_cast<void *>(0));
    backing = whitebox_find_backing(owner.pool, memory);
    REQUIRE(backing != static_cast<WhiteboxBacking *>(0));
    original_object = backing->object;
    original_references = backing->references;

    backing->object = static_cast<void *>(0);
    breakpoints_before = InterlockedCompareExchange(&whitebox_breakpoints, 0, 0);
    {
        BreakpointScope breakpoint_scope;
        REQUIRE(breakpoint_scope.handle != static_cast<PVOID>(0));
        Mempool_Free(memory);
    }
    REQUIRE(InterlockedCompareExchange(&whitebox_breakpoints, 0, 0) >
            breakpoints_before);
    REQUIRE(backing->references == original_references);
    REQUIRE(backing->closing == 0);

    backing->object = reinterpret_cast<void *>(static_cast<std::uintptr_t>(1U));
    breakpoints_before = InterlockedCompareExchange(&whitebox_breakpoints, 0, 0);
    {
        BreakpointScope breakpoint_scope;
        REQUIRE(breakpoint_scope.handle != static_cast<PVOID>(0));
        Mempool_Free(memory);
    }
    REQUIRE(InterlockedCompareExchange(&whitebox_breakpoints, 0, 0) >
            breakpoints_before);
    REQUIRE(backing->references == original_references);
    REQUIRE(backing->closing == 0);

    backing->object = original_object;
    Mempool_Free(memory);
}

TEST_CASE("destroy bounds corrupt owner and dual-hash traversals before free",
          "[mempool][metadata][whitebox][destroy]")
{
    const DestroyCorruption scenarios[] = {
        DestroyCorruption::OwnerCycle,
        DestroyCorruption::CountSmall,
        DestroyCorruption::CountLarge,
        DestroyCorruption::CountHugeCycle,
        DestroyCorruption::HeadPrevious,
        DestroyCorruption::CrossRegistration,
        DestroyCorruption::BaseHashCycle,
        DestroyCorruption::IdentityHashCycle
    };
    const std::size_t scenario_count =
        sizeof(scenarios) / sizeof(scenarios[0]);
    std::size_t index = 0U;

    while (index < scenario_count) {
        INFO("corruption scenario index " << index);
        REQUIRE(whitebox_destroy_rejects(scenarios[index]));
        ++index;
    }
}

TEST_CASE("free retirement rejects corrupt local and dual-hash links",
          "[mempool][metadata][whitebox][free]")
{
    const FreeCorruption scenarios[] = {
        FreeCorruption::HeadPrevious,
        FreeCorruption::BaseHashCycle,
        FreeCorruption::IdentityHashCycle
    };
    const std::size_t scenario_count =
        sizeof(scenarios) / sizeof(scenarios[0]);
    std::size_t index = 0U;

    while (index < scenario_count) {
        INFO("free corruption scenario index " << index);
        REQUIRE(whitebox_free_retire_rejects(scenarios[index]));
        ++index;
    }
}

TEST_CASE("backing publication rejects a corrupt target hash bucket",
          "[mempool][metadata][whitebox][publish]")
{
    REQUIRE(whitebox_publish_rejects_corrupt_base_bucket());
}

TEST_CASE("registration cycles reject admit free destroy and publication",
          "[mempool][metadata][whitebox][registry]")
{
    REQUIRE(whitebox_registration_cycle_rejected());
    REQUIRE(whitebox_create_rejects_corrupt_registration_bucket());
}

#if defined(MEMPOOL_TESTING)
TEST_CASE("registration count corruption fails closed without index changes",
          "[mempool][metadata][whitebox][registry][counts]")
{
    const MEMPOOL_REGISTRY_TEST_CORRUPTION scenarios[] = {
        MEMPOOL_REGISTRY_TEST_GLOBAL_COUNT_SMALL,
        MEMPOOL_REGISTRY_TEST_GLOBAL_COUNT_LARGE,
        MEMPOOL_REGISTRY_TEST_BUCKET_COUNT_SMALL,
        MEMPOOL_REGISTRY_TEST_BUCKET_COUNT_LARGE
    };
    const std::size_t scenario_count =
        sizeof(scenarios) / sizeof(scenarios[0]);

    for (std::size_t index = 0U; index < scenario_count; ++index) {
        INFO("registration count corruption scenario index " << index);
        REQUIRE(whitebox_registration_count_rejected(scenarios[index]));
    }
}
#endif
