/*
 * Complete user-mode Mempool example.
 *
 * Build (MinGW):
 *
 *   gcc -std=gnu11 -D_WIN32 -w -c libc.c -o libc_demo.o
 *   gcc -std=gnu89 -D_WIN32 -Wall -Wextra -Wconversion -Wshadow -Werror \
 *       mempool_demo.c mempool.c allocator.c libc_demo.o -o mempool_demo.exe
 *
 * libc.c contains a couple of implementation-only C99 loop declarations;
 * compile that private CRT object with its supported dialect, while this
 * demo and the pool sources keep the old-MSVC declaration discipline.
 *
 * Mempool owns the backing pages and returns fixed-size cells for ordinary
 * requests.  Requests close to a page are stored in page-aligned large
 * chunks.  The caller never needs to know which path was selected:
 *
 *     Mempool_CreatePool -> Mempool_Alloc -> Mempool_Free
 *                                      |
 *                           Mempool_DestroyPool
 *
 * All locals are initialized at declaration time and declarations remain at
 * the beginning of each function for old MSVC/C89 compatibility.
 */

#include "mempool.h"

#include <stdio.h>

static int demo_small_allocations(MEMPOOL *pool)
{
    void *blocks[32] = {0};
    ULONG sizes[32] = {0};
    unsigned int count = 0U;
    unsigned int index = 0U;
    unsigned char *bytes = (unsigned char *)0;

    sizes[0] = 1UL;
    sizes[1] = 16UL;
    sizes[2] = 127UL;
    sizes[3] = 128UL;
    sizes[4] = 129UL;
    sizes[5] = 512UL;
    sizes[6] = 1024UL;
    sizes[7] = 2048UL;
    count = 8U;

    printf("[small allocations]\n");
    index = 0U;
    while (index < count) {
        blocks[index] = Mempool_Alloc(pool, sizes[index]);
        if (blocks[index] == (void *)0) {
            printf("  allocation %u (%lu bytes) failed\n", index,
                   (unsigned long)sizes[index]);
            while (index > 0U) {
                --index;
                Mempool_Free(blocks[index]);
            }
            return 0;
        }
        bytes = (unsigned char *)blocks[index];
        bytes[0] = (unsigned char)(0x10U + index);
        bytes[sizes[index] - 1UL] = (unsigned char)(0xE0U + index);
        printf("  %lu bytes -> %p, first=0x%02X last=0x%02X\n",
               (unsigned long)sizes[index], blocks[index],
               (unsigned int)bytes[0],
               (unsigned int)bytes[sizes[index] - 1UL]);
        ++index;
    }

    index = count;
    while (index > 0U) {
        --index;
        Mempool_Free(blocks[index]);
    }
    printf("  all small blocks released\n");
    return 1;
}

static int demo_multi_page_reuse(MEMPOOL *pool)
{
    void *blocks[700] = {0};
    unsigned int count = 700U;
    unsigned int index = 0U;
    unsigned int second_pass = 0U;

    printf("[multi-page growth and reclamation]\n");
    /* 700 x 64 bytes is larger than one user-mode 64 KiB backing page. */
    while (index < count) {
        blocks[index] = Mempool_Alloc(pool, 64UL);
        if (blocks[index] == (void *)0) {
            printf("  allocation %u failed\n", index);
            while (index > 0U) {
                --index;
                Mempool_Free(blocks[index]);
            }
            return 0;
        }
        ++index;
    }
    printf("  allocated %u blocks across multiple pages\n", count);
    index = 0U;
    while (index < count) {
        Mempool_Free(blocks[index]);
        ++index;
    }
    printf("  released all data blocks; empty data pages become reclaimable\n");

    /* The pool remains usable after those pages have been reclaimed. */
    second_pass = 0U;
    while (second_pass < 16U) {
        blocks[second_pass] = Mempool_Alloc(pool, 64UL);
        if (blocks[second_pass] == (void *)0) {
            while (second_pass > 0U) {
                --second_pass;
                Mempool_Free(blocks[second_pass]);
            }
            return 0;
        }
        ++second_pass;
    }
    while (second_pass > 0U) {
        --second_pass;
        Mempool_Free(blocks[second_pass]);
    }
    printf("  pool remained usable after page reclamation\n");
    return 1;
}

static int demo_large_allocation(MEMPOOL *pool)
{
    void *memory = (void *)0;
    unsigned char *bytes = (unsigned char *)0;
    ULONG_PTR address = 0;

    printf("[large allocation]\n");
    /* This request is handled by the page-aligned large-chunk path. */
    memory = Mempool_Alloc(pool, 65536UL);
    if (memory == (void *)0) {
        printf("  large allocation failed\n");
        return 0;
    }
    address = (ULONG_PTR)memory;
    bytes = (unsigned char *)memory;
    bytes[0] = 0x3CU;
    bytes[65535U] = 0xC3U;
    printf("  65536 bytes -> %p, page offset=0x%lX, edge bytes=0x%02X/0x%02X\n",
           memory, (unsigned long)(address & 0xFFFFUL),
           (unsigned int)bytes[0], (unsigned int)bytes[65535U]);
    Mempool_Free(memory);
    printf("  large block released\n");
    return 1;
}

int main(void)
{
    MEMPOOL *pool = (MEMPOOL *)0;
    ULONG released_pages = 0UL;
    int result = 0;

    printf("=== Mempool user-mode demo ===\n");
    printf("Mempool hides page lists, bitmap cells and large chunks.\n\n");

    pool = Mempool_CreatePool(MEMPOOL_NONPAGED);
    if (pool == (MEMPOOL *)0) {
        printf("Mempool_CreatePool failed\n");
        return 1;
    }

    /* Zero-size requests are rejected without changing pool state. */
    if (Mempool_Alloc(pool, 0UL) != (void *)0) {
        printf("unexpected success for zero-size allocation\n");
        result = 2;
        goto cleanup;
    }
    printf("zero-size allocation correctly returned NULL\n\n");

    if (!demo_small_allocations(pool)) {
        result = 3;
        goto cleanup;
    }
    printf("\n");
    if (!demo_large_allocation(pool)) {
        result = 4;
        goto cleanup;
    }
    printf("\n");
    if (!demo_multi_page_reuse(pool)) {
        result = 5;
        goto cleanup;
    }

cleanup:
    /* Destroy is the final owner operation; every returned pointer is invalid
       after this call.  It also releases outstanding allocations if any. */
    released_pages = Mempool_DestroyPool(pool);
    printf("\nMempool_DestroyPool released %lu page(s), result=%d\n",
           (unsigned long)released_pages, result);
    return result;
}
