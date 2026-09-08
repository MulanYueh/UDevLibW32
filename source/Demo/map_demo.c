/*
 * User-mode map demonstration.
 *
 * Build with the C implementation and the platform allocator:
 *
 *   gcc -std=c11 -Wall -Wextra -Wconversion -Wshadow -Werror -I. \
 *       map_demo.c map.c allocator.c -o map_demo.exe
 *
 * The same source can be compiled by MSVC together with map.c and
 * allocator.c.  Kernel-mode callers use the same map API; only the allocator
 * backend selected by allocator.c changes.
 */

#include "allocator.h"
#include "map.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct demo_record_t {
    int id;
    int score;
} demo_record_t;

typedef struct demo_allocator_state_t {
    size_t allocations;
    size_t frees;
} demo_allocator_state_t;

/* The map copies keys, so this callback includes the terminating byte. */
static int demo_key_size(const void *key)
{
    const char *text = (const char *)key;
    size_t length = 0U;

    if (text == (const char *)0) {
        return 0;
    }
    length = strlen(text) + 1U;
    if (length > (size_t)INT_MAX) {
        return -1;
    }
    return (int)length;
}

static unsigned int demo_hash(const void *key, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)key;
    unsigned int hash = 2166136261U;

    while (size != 0U) {
        hash ^= (unsigned int)*bytes++;
        hash *= 16777619U;
        --size;
    }
    return hash;
}

static map_bool_t demo_match(const void *left, const void *right)
{
    if (left == (const void *)0 || right == (const void *)0) {
        return left == right ? MAP_TRUE : MAP_FALSE;
    }
    return strcmp((const char *)left, (const char *)right) == 0
               ? MAP_TRUE
               : MAP_FALSE;
}

/* Demonstrate how a caller can account for, or wrap, the default allocator. */
static void *demo_alloc(void *context, size_t size)
{
    demo_allocator_state_t *state = (demo_allocator_state_t *)context;
    void *memory = (void *)0;

    if (state == (demo_allocator_state_t *)0 || size == 0U) {
        return (void *)0;
    }
    memory = Allocator_Malloc(size);
    if (memory != (void *)0) {
        ++state->allocations;
    }
    return memory;
}

static void demo_free(void *context, void *pointer)
{
    demo_allocator_state_t *state = (demo_allocator_state_t *)context;

    if (pointer == (void *)0) {
        return;
    }
    Allocator_Free(pointer);
    if (state != (demo_allocator_state_t *)0) {
        ++state->frees;
    }
}

static int demo_check(int condition, const char *message)
{
    if (condition == MAP_FALSE) {
        fprintf(stderr, "map demo error: %s\n", message);
        return 0;
    }
    return 1;
}

int main(void)
{
    map_base_t map;
    demo_allocator_state_t allocator_state = {0U, 0U};
    demo_record_t alice = {1001, 98};
    demo_record_t alice_backup = {1002, 91};
    demo_record_t bob = {2001, 87};
    demo_record_t carol = {3001, 95};
    demo_record_t *stored = (demo_record_t *)0;
    demo_record_t taken = {0, 0};
    map_iter_t iterator;
    void *external_value = (void *)0;
    char external_text[] = "caller-owned value";
    char mutable_key[16] = "carol";
    int alice_matches = 0;
    int remaining = 0;
    int status = 1;

    printf("map demo (user mode)\n");

    map_init(&map, (void *)0);
    if (!demo_check(map_set_allocator(&map, &allocator_state, demo_alloc,
                                       demo_free),
                    "map_set_allocator failed")) {
        goto cleanup;
    }
    if (!demo_check(map_set_key_functions(&map, demo_key_size, demo_hash,
                                         demo_match),
                    "map_set_key_functions failed")) {
        goto cleanup;
    }

    stored = (demo_record_t *)map_insert(&map, "alice", &alice,
                                         sizeof(alice));
    if (!demo_check(stored != (demo_record_t *)0,
                    "could not insert alice")) {
        goto cleanup;
    }
    stored->score = 99;
    printf("inserted alice: id=%d score=%d (value is stored inline)\n",
           stored->id, stored->score);

    /* append keeps this duplicate behind the first alice node. */
    if (!demo_check(map_append(&map, "alice", &alice_backup,
                               sizeof(alice_backup)) != (void *)0,
                    "could not append duplicate alice")) {
        goto cleanup;
    }
    if (!demo_check(map_insert(&map, "bob", &bob, sizeof(bob)) != (void *)0,
                    "could not insert bob")) {
        goto cleanup;
    }

    /* Keys are copied into the map; changing the source does not change it. */
    if (!demo_check(map_insert(&map, mutable_key, &carol, sizeof(carol)) !=
                        (void *)0,
                    "could not insert carol")) {
        goto cleanup;
    }
    strcpy(mutable_key, "changed");
    if (!demo_check(map_get(&map, "carol") != (void *)0,
                    "copied key was unexpectedly changed")) {
        goto cleanup;
    }

    stored = (demo_record_t *)map_get(&map, "alice");
    if (!demo_check(stored != (demo_record_t *)0,
                    "could not find alice")) {
        goto cleanup;
    }
    printf("lookup alice: id=%d score=%d\n", stored->id, stored->score);

    if (!demo_check(map_take(&map, "bob", &taken, sizeof(taken)),
                    "could not take bob")) {
        goto cleanup;
    }
    printf("take bob: id=%d score=%d (node removed)\n", taken.id,
           taken.score);

    /* A zero value size stores an opaque pointer owned by the caller. */
    if (!demo_check(map_insert(&map, "message", external_text, 0U) ==
                        (void *)external_text,
                    "could not insert external value")) {
        goto cleanup;
    }
    if (!demo_check(map_take(&map, "message", &external_value, 0U),
                    "could not take external value")) {
        goto cleanup;
    }
    printf("external value: %s\n", (const char *)external_value);

    /* Filtered iteration finds both duplicate alice values. */
    iterator = map_key_iter(&map, "alice");
    while (map_next(&map, &iterator) != MAP_FALSE) {
        stored = (demo_record_t *)iterator.value;
        ++alice_matches;
        printf("alice iterator: id=%d score=%d\n", stored->id,
               stored->score);
        if (alice_matches == 1) {
            (void)map_erase(&map, &iterator);
        }
    }
    if (!demo_check(alice_matches == 2,
                    "filtered iterator did not find both alice nodes")) {
        goto cleanup;
    }

    stored = (demo_record_t *)map_get(&map, "alice");
    if (!demo_check(stored != (demo_record_t *)0 &&
                        stored->id == alice_backup.id,
                    "iterator erase did not remove the first alice")) {
        goto cleanup;
    }

    iterator = map_iter();
    while (map_next(&map, &iterator) != MAP_FALSE) {
        stored = (demo_record_t *)iterator.value;
        ++remaining;
        printf("remaining entry: key=%s id=%d score=%d\n",
               (const char *)iterator.key, stored->id, stored->score);
    }
    if (!demo_check(remaining == 2,
                    "unexpected number of entries after removals")) {
        goto cleanup;
    }

    if (!demo_check(map_resize(&map, 17), "explicit map resize failed")) {
        goto cleanup;
    }
    stored = (demo_record_t *)map_get(&map, "carol");
    if (!demo_check(stored != (demo_record_t *)0,
                    "carol disappeared after resize")) {
        goto cleanup;
    }
    printf("resized map to %d buckets; carol still has id=%d\n",
           map.nbuckets, stored->id);

    map_remove(&map, "carol");
    if (!demo_check(map_get(&map, "carol") == (void *)0,
                    "map_remove did not remove carol")) {
        goto cleanup;
    }

    status = 0;

cleanup:
    map_clear(&map);
    printf("allocator blocks: allocated=%zu freed=%zu\n",
           allocator_state.allocations, allocator_state.frees);
    if (allocator_state.allocations != allocator_state.frees) {
        fprintf(stderr, "map demo error: allocator blocks are unbalanced\n");
        status = 1;
    }
    return status;
}
