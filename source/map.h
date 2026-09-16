/*
 * Copyright 2021 David Xanatos, xanasoft.com
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * Small separate-chaining hash map.
 *
 * The map owns its nodes and copies keys. Values are either copied into the
 * node (vsize != 0) or stored as an opaque caller-owned pointer (vsize == 0).
 * The latter convention is retained for compatibility with the historical
 * implementation.
 *
 * The header deliberately uses only C language types. It can therefore be
 * included from C or C++, with or without Windows SDK/WDK headers. A map is
 * not internally synchronized; protect a shared instance with the project's
 * lock primitives.
 * Link map.c with allocator.c because the default/legacy map_alloc and
 * map_free entry points are part of this ABI. Custom callbacks can still be
 * installed with map_set_allocator to bypass the default allocator at run
 * time.
 */
#ifndef MEMORY_POOL_MAP_H_INCLUDED
#define MEMORY_POOL_MAP_H_INCLUDED

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char map_bool_t;

#ifndef MAP_INLINE
#if defined(__cplusplus)
#define MAP_INLINE static inline
#elif defined(_MSC_VER)
#define MAP_INLINE static __inline
#elif defined(__GNUC__) || defined(__clang__)
#define MAP_INLINE static __inline__
#else
#define MAP_INLINE static
#endif
#endif

#ifndef MAP_FALSE
#define MAP_FALSE ((map_bool_t)0)
#endif
#ifndef MAP_TRUE
#define MAP_TRUE ((map_bool_t)1)
#endif

struct map_node_t;
typedef struct map_node_t map_node_t;

typedef void *(*map_malloc_fn)(void *context, size_t size);
typedef void (*map_free_fn)(void *context, void *ptr);
typedef int (*map_key_size_fn)(const void *key);
typedef unsigned int (*map_hash_fn)(const void *key, size_t size);
typedef map_bool_t (*map_match_fn)(const void *key1, const void *key2);

/*
 * Keep the original field order and scalar widths for source/binary users
 * that embed or statically initialize map_base_t. New code should use the
 * callback typedefs and map_set_allocator/map_set_key_functions.
 */
typedef struct map_base_t {
    map_node_t **buckets;
    int nbuckets;
    int nnodes;

    void *mem_pool;
    map_malloc_fn func_malloc;
    map_free_fn func_free;

    map_key_size_fn func_key_size;
    map_hash_fn func_hash_key;
    map_match_fn func_match_key;
} map_base_t;

typedef map_base_t HASH_MAP;

/* Legacy allocator entry points retained for callers that used them directly. */
void *map_alloc(void *pool, size_t size);
void map_free(void *pool, void *ptr);

/* Windows wchar_t is UTF-16; the helpers also work with other wchar_t widths.
 * map_wcsihash is the case-insensitive hash that pairs with map_wcsimatch. */
int map_wcssize(const void *key);
map_bool_t map_wcsimatch(const void *key1, const void *key2);
unsigned int map_wcsihash(const void *key, size_t size);

/* `pool` is passed to custom allocator callbacks as their context. The
 * built-in path calls allocator.c: user mode uses the process heap (or CRT
 * fallback), while kernel mode uses NonPagedPool by default. Define
 * MAP_USE_NONPAGED_POOL=0 when compiling map.c to select paged pool. A
 * freestanding kernel build without WDK headers must install callbacks. */
void map_init(map_base_t *m, void *pool);
/* Call before the first insertion. Both callbacks NULL restore the default. */
map_bool_t map_set_allocator(map_base_t *m, void *pool,
                             map_malloc_fn alloc_fn, map_free_fn free_fn);
map_bool_t map_set_key_functions(map_base_t *m, map_key_size_fn size_fn,
                                 map_hash_fn hash_fn, map_match_fn match_fn);

map_bool_t map_resize(map_base_t *m, int nbuckets);
/* Insertions intentionally allow duplicate keys. map_insert puts the new
 * node at the bucket head; map_append puts it at the bucket tail. */
void *map_add(map_base_t *m, const void *key, void *vdata, size_t vsize,
              map_bool_t append);

MAP_INLINE void *map_insert(map_base_t *m, const void *key, void *vdata,
                            size_t vsize)
{
    return map_add(m, key, vdata, vsize, MAP_FALSE);
}

MAP_INLINE void *map_append(map_base_t *m, const void *key, void *vdata,
                            size_t vsize)
{
    return map_add(m, key, vdata, vsize, MAP_TRUE);
}

void *map_get(map_base_t *m, const void *key);
/* With vsize > 0, vdata receives a checked copy of an inline value. With
 * vsize == 0, vdata must be a void** and receives an external pointer; copied
 * values are rejected because their storage is released with the node. */
map_bool_t map_take(map_base_t *m, const void *key, void *vdata,
                    size_t vsize);

MAP_INLINE void map_remove(map_base_t *m, const void *key)
{
    (void)map_take(m, key, NULL, 0);
}

void map_clear(map_base_t *m);

/*
 * Iterators remain source-compatible with the old five-field public type.
 * The fields after value are implementation state; callers should treat the
 * whole object as opaque and use map_iter/map_next/map_erase only.
 */
typedef struct map_iter_t {
    int bucketIdx;
    map_node_t *node;
    int ksize;
    const void *key;
    void *value;

    map_base_t *owner;
    map_node_t *next_node;
    map_node_t *current;
    const void *query_key;
    void *query_pointer;
    size_t query_size;
    unsigned int query_hash;
    map_bool_t key_filter;
} map_iter_t;

map_iter_t map_iter(void);
map_iter_t map_key_iter(map_base_t *m, const void *key);
map_bool_t map_next(map_base_t *m, map_iter_t *iter);
map_bool_t map_erase(map_base_t *m, map_iter_t *iter);

/* Historical wide-string pointer-key helpers. */
map_bool_t str_map_match(const void *key1, const void *key2);
unsigned int str_map_hash(const void *key, size_t size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MEMORY_POOL_MAP_H_INCLUDED */
