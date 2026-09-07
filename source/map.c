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
 * Portable hash map implementation.
 *
 * The implementation does not call the C string/memory routines. This keeps
 * the data-structure code usable in freestanding Windows driver builds. The
 * default allocator is supplied by allocator.c, which selects the correct
 * Windows pool API for the target WDK version.
 */
#include "map.h"
#include "def.h"
#include "allocator.h"

#define MAP_INITIAL_BUCKETS 8
#define MAP_MAX_INT 2147483647

#if defined(_KERNEL_MODE) && !defined(MAP_USE_NONPAGED_POOL)
#define MAP_USE_NONPAGED_POOL 1
#endif

struct map_node_t {
    unsigned int hash;
    void *value;
    map_node_t *next;
    size_t key_size;
    size_t value_size;
    map_bool_t value_inline;
    unsigned char data[1];
};

static void *map_default_alloc(void *context, size_t size)
{
    (void)context;
#if defined(_KERNEL_MODE)
    return Allocator_Malloc((BOOLEAN)(MAP_USE_NONPAGED_POOL != 0), size,
                            (ULONG)MAP_POOL_TAG);
#else
    return Allocator_Malloc(size);
#endif
}

static void map_default_free(void *context, void *ptr)
{
    (void)context;
#if defined(_KERNEL_MODE)
    Allocator_Free(ptr, (ULONG)MAP_POOL_TAG);
#else
    Allocator_Free(ptr);
#endif
}

void *map_alloc(void *pool, size_t size)
{
    return map_default_alloc(pool, size);
}

void map_free(void *pool, void *ptr)
{
    map_default_free(pool, ptr);
}

static void map_zero(void *dst, size_t size)
{
    unsigned char *out = (unsigned char *)dst;
    while (size != 0) {
        *out++ = 0;
        --size;
    }
}

static void map_copy(void *dst, const void *src, size_t size)
{
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    while (size != 0) {
        *out++ = *in++;
        --size;
    }
}

static int map_equal(const void *left, const void *right, size_t size)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    while (size != 0) {
        if (*a++ != *b++) {
            return 0;
        }
        --size;
    }
    return 1;
}

static unsigned int map_hash_bytes(const void *key, size_t size)
{
    const unsigned char *ptr = (const unsigned char *)key;
    unsigned int hash = 5381U;
    while (size != 0) {
        hash = ((hash << 5) + hash) ^ (unsigned int)*ptr++;
        --size;
    }
    return hash;
}

static size_t map_align(size_t size)
{
    const size_t mask = sizeof(void *) - 1U;
    if (size > (size_t)-1 - mask) {
        return 0;
    }
    return (size + mask) & ~mask;
}

static const unsigned char *map_node_key(const map_node_t *node)
{
    return node ? node->data : (const unsigned char *)0;
}

static void *map_node_value(const map_node_t *node)
{
    const unsigned char *value = (const unsigned char *)0;
    if (!node) {
        return NULL;
    }
    if (!node->value_inline) {
        return node->value;
    }
    value = node->data + map_align(node->key_size);
    return (void *)value;
}

typedef struct map_lookup_t {
    const void *bytes;
    const void *match_key;
    void *pointer_key;
    size_t size;
    unsigned int hash;
} map_lookup_t;

/* In the default mode a key is a pointer value, not the pointed-to bytes. */
static map_bool_t map_make_lookup(const map_base_t *m, const void *key,
                                  map_lookup_t *lookup)
{
    int key_size = 0;
    if (!m || !lookup) {
        return MAP_FALSE;
    }
    lookup->pointer_key = NULL;
    if (m->func_key_size) {
        key_size = m->func_key_size(key);
        if (key_size < 0) {
            return MAP_FALSE;
        }
        lookup->bytes = key;
        lookup->match_key = key;
        lookup->size = (size_t)key_size;
    } else {
        lookup->pointer_key = (void *)key;
        lookup->bytes = &lookup->pointer_key;
        lookup->match_key = lookup->bytes;
        lookup->size = sizeof(lookup->pointer_key);
    }
    lookup->hash = m->func_hash_key
        ? m->func_hash_key(lookup->bytes, lookup->size)
        : map_hash_bytes(lookup->bytes, lookup->size);
    return MAP_TRUE;
}

static int map_bucket_index(const map_base_t *m, unsigned int hash)
{
    unsigned int buckets = 0U;
    if (!m || m->nbuckets <= 0) {
        return -1;
    }
    buckets = (unsigned int)m->nbuckets;
    if ((buckets & (buckets - 1U)) == 0U) {
        return (int)(hash & (buckets - 1U));
    }
    return (int)(hash % buckets);
}

static map_bool_t map_node_matches(const map_base_t *m,
                                   const map_node_t *node,
                                   const map_lookup_t *lookup)
{
    if (!m || !node || !lookup || node->hash != lookup->hash) {
        return MAP_FALSE;
    }
    if (m->func_match_key) {
        return m->func_match_key(map_node_key(node), lookup->match_key)
            ? MAP_TRUE : MAP_FALSE;
    }
    if (node->key_size != lookup->size) {
        return MAP_FALSE;
    }
    return map_equal(map_node_key(node), lookup->bytes, lookup->size)
        ? MAP_TRUE : MAP_FALSE;
}

static map_node_t **map_find_ref(map_base_t *m, const map_lookup_t *lookup,
                                 int *bucket_index)
{
    map_node_t **link = (map_node_t **)0;
    if (!m || !lookup || !m->buckets || m->nbuckets <= 0) {
        return NULL;
    }
    *bucket_index = map_bucket_index(m, lookup->hash);
    if (*bucket_index < 0) {
        return NULL;
    }
    link = &m->buckets[*bucket_index];
    while (*link) {
        if (map_node_matches(m, *link, lookup)) {
            return link;
        }
        link = &(*link)->next;
    }
    return NULL;
}

static void map_link_node(map_base_t *m, map_node_t *node, map_bool_t append)
{
    map_node_t **link = (map_node_t **)0;
    int bucket = -1;

    if (!m || !node || !m->buckets || m->nbuckets <= 0) {
        return;
    }
    bucket = map_bucket_index(m, node->hash);
    link = &m->buckets[bucket];
    if (append) {
        while (*link) {
            link = &(*link)->next;
        }
    }
    node->next = *link;
    *link = node;
}

static map_node_t *map_new_node(map_base_t *m, const void *key, void *vdata,
                                size_t vsize)
{
    map_lookup_t lookup = {0};
    size_t key_storage = 0;
    size_t total = 0;
    map_node_t *node = (map_node_t *)0;
    if (!m || !m->func_malloc) {
        return NULL;
    }
    if (!map_make_lookup(m, key, &lookup)) {
        return NULL;
    }
    key_storage = map_align(lookup.size);
    if (lookup.size != 0 && key_storage == 0) {
        return NULL;
    }
    if (key_storage > (size_t)-1 - vsize ||
        sizeof(*node) > (size_t)-1 - key_storage - vsize) {
        return NULL;
    }
    total = sizeof(*node) + key_storage + vsize;
    node = (map_node_t *)m->func_malloc(m->mem_pool, total);
    if (!node) {
        return NULL;
    }
    node->hash = lookup.hash;
    node->next = NULL;
    node->key_size = lookup.size;
    node->value_size = vsize;
    node->value_inline = vsize != 0 ? MAP_TRUE : MAP_FALSE;
    if (lookup.size != 0) {
        map_copy(node->data, lookup.bytes, lookup.size);
    }
    if (vsize != 0) {
        node->value = node->data + key_storage;
        if (vdata) {
            map_copy(node->value, vdata, vsize);
        } else {
            map_zero(node->value, vsize);
        }
    } else {
        node->value = vdata;
    }
    return node;
}

static map_bool_t map_ensure_capacity(map_base_t *m)
{
    int next = 0;
    if (!m || m->nnodes < 0 || m->nnodes >= MAP_MAX_INT) {
        return MAP_FALSE;
    }
    if (m->nbuckets <= 0) {
        return map_resize(m, MAP_INITIAL_BUCKETS);
    }
    /* Keep the average chain length at or below two nodes. */
    if ((size_t)m->nnodes + 1U <= (size_t)m->nbuckets * 2U) {
        return MAP_TRUE;
    }
    if (m->nbuckets > MAP_MAX_INT / 2) {
        return MAP_FALSE;
    }
    next = m->nbuckets * 2;
    return map_resize(m, next);
}

void map_init(map_base_t *m, void *pool)
{
    if (!m) {
        return;
    }
    map_zero(m, sizeof(*m));
    m->mem_pool = pool;
    m->func_malloc = map_alloc;
    m->func_free = map_free;
    m->func_hash_key = map_hash_bytes;
}

map_bool_t map_set_allocator(map_base_t *m, void *pool,
                             map_malloc_fn alloc_fn, map_free_fn free_fn)
{
    if (!m || m->nnodes != 0 || m->buckets || (alloc_fn && !free_fn) ||
        (!alloc_fn && free_fn)) {
        return MAP_FALSE;
    }
    m->mem_pool = pool;
    m->func_malloc = alloc_fn ? alloc_fn : map_alloc;
    m->func_free = free_fn ? free_fn : map_free;
    return MAP_TRUE;
}

map_bool_t map_set_key_functions(map_base_t *m, map_key_size_fn size_fn,
                                 map_hash_fn hash_fn, map_match_fn match_fn)
{
    if (!m || m->nnodes != 0) {
        return MAP_FALSE;
    }
    m->func_key_size = size_fn;
    m->func_hash_key = hash_fn ? hash_fn : map_hash_bytes;
    m->func_match_key = match_fn;
    return MAP_TRUE;
}

map_bool_t map_resize(map_base_t *m, int nbuckets)
{
    map_node_t **new_buckets = (map_node_t **)0;
    map_node_t *node = (map_node_t *)0;
    map_node_t *next = (map_node_t *)0;
    size_t bytes = 0;
    int i = 0;
    if (!m || m->nbuckets < 0 || nbuckets <= 0 || !m->func_malloc || !m->func_free ||
        nbuckets > MAP_MAX_INT) {
        return MAP_FALSE;
    }
    if ((size_t)nbuckets > (size_t)-1 / sizeof(*new_buckets)) {
        return MAP_FALSE;
    }
    bytes = (size_t)nbuckets * sizeof(*new_buckets);
    new_buckets = (map_node_t **)m->func_malloc(m->mem_pool, bytes);
    if (!new_buckets) {
        return MAP_FALSE;
    }
    map_zero(new_buckets, bytes);

    /* Rehash only after allocation succeeds, so failure leaves the map intact. */
    for (i = 0; i < m->nbuckets; ++i) {
        node = m->buckets ? m->buckets[i] : NULL;
        while (node) {
            next = node->next;
            node->next = NULL;
            /* The new array is a power of two in normal growth, but arbitrary
             * user-requested sizes are supported by map_bucket_index as well. */
            {
                unsigned int bucket = (nbuckets & (nbuckets - 1))
                    ? node->hash % (unsigned int)nbuckets
                    : node->hash & ((unsigned int)nbuckets - 1U);
                node->next = new_buckets[bucket];
                new_buckets[bucket] = node;
            }
            node = next;
        }
    }
    if (m->buckets) {
        m->func_free(m->mem_pool, m->buckets);
    }
    m->buckets = new_buckets;
    m->nbuckets = nbuckets;
    return MAP_TRUE;
}

void *map_add(map_base_t *m, const void *key, void *vdata, size_t vsize,
              map_bool_t append)
{
    map_node_t *node = (map_node_t *)0;
    if (!m || !m->func_malloc || !m->func_free) {
        return NULL;
    }
    if (!map_ensure_capacity(m)) {
        return NULL;
    }
    node = map_new_node(m, key, vdata, vsize);
    if (!node) {
        return NULL;
    }
    map_link_node(m, node, append);
    ++m->nnodes;
    return map_node_value(node);
}

void *map_get(map_base_t *m, const void *key)
{
    map_lookup_t lookup = {0};
    map_node_t **ref = (map_node_t **)0;
    int bucket = -1;

    if (!m) {
        return NULL;
    }
    if (!map_make_lookup(m, key, &lookup)) {
        return NULL;
    }
    ref = map_find_ref(m, &lookup, &bucket);
    return ref ? map_node_value(*ref) : NULL;
}

static map_bool_t map_remove_ref(map_base_t *m, map_node_t **ref)
{
    map_node_t *node = (map_node_t *)0;
    if (!m || !ref || !*ref || !m->func_free) {
        return MAP_FALSE;
    }
    node = *ref;
    *ref = node->next;
    m->func_free(m->mem_pool, node);
    if (m->nnodes > 0) {
        --m->nnodes;
    }
    return MAP_TRUE;
}

map_bool_t map_take(map_base_t *m, const void *key, void *vdata, size_t vsize)
{
    map_lookup_t lookup = {0};
    map_node_t **ref = (map_node_t **)0;
    map_node_t *node = (map_node_t *)0;
    int bucket = -1;
    if (!map_make_lookup(m, key, &lookup)) {
        if (vdata && vsize == 0) {
            *(void **)vdata = NULL;
        }
        return MAP_FALSE;
    }
    ref = map_find_ref(m, &lookup, &bucket);
    if (!ref) {
        if (vdata && vsize == 0) {
            *(void **)vdata = NULL;
        }
        return MAP_FALSE;
    }
    node = *ref;
    if (vdata) {
        if (vsize == 0) {
            if (node->value_inline) {
                /* A copied value dies with its node; exposing it after remove
                 * would return a dangling pointer. */
                return MAP_FALSE;
            }
            *(void **)vdata = map_node_value(node);
        } else {
            /* Inline values carry their size, so reject truncating/overflowing
             * copies. External pointer values retain the legacy unsized API. */
            if (node->value_inline && vsize > node->value_size) {
                return MAP_FALSE;
            }
            if (!map_node_value(node)) {
                return MAP_FALSE;
            }
            map_copy(vdata, map_node_value(node), vsize);
        }
    }
    return map_remove_ref(m, ref);
}

void map_clear(map_base_t *m)
{
    map_node_t *node = (map_node_t *)0;
    map_node_t *next = (map_node_t *)0;
    int i = 0;
    if (!m) {
        return;
    }
    if (m->buckets && m->func_free) {
        for (i = 0; i < m->nbuckets; ++i) {
            node = m->buckets[i];
            while (node) {
                next = node->next;
                m->func_free(m->mem_pool, node);
                node = next;
            }
        }
        m->func_free(m->mem_pool, m->buckets);
    }
    m->buckets = NULL;
    m->nbuckets = 0;
    m->nnodes = 0;
}

map_iter_t map_iter(void)
{
    map_iter_t iter = {0};
    map_zero(&iter, sizeof(iter));
    iter.bucketIdx = -1;
    return iter;
}

map_iter_t map_key_iter(map_base_t *m, const void *key)
{
    map_iter_t iter = {0};
    iter = map_iter();
    iter.owner = m;
    /* A NULL key historically means an unrestricted iterator. */
    if (m && key) {
        map_lookup_t lookup = {0};
        iter.key_filter = MAP_TRUE;
        if (map_make_lookup(m, key, &lookup)) {
            iter.query_key = key;
            iter.query_pointer = lookup.pointer_key;
            iter.query_size = lookup.size;
            iter.query_hash = lookup.hash;
            iter.ksize = lookup.size > (size_t)MAP_MAX_INT
                ? MAP_MAX_INT : (int)lookup.size;
            iter.bucketIdx = map_bucket_index(m, lookup.hash);
            iter.next_node = (iter.bucketIdx >= 0 && m->buckets)
                ? m->buckets[iter.bucketIdx] : NULL;
        } else {
            /* A negative size marks an invalid query without turning it into
             * an unrestricted iterator. */
            iter.ksize = -1;
        }
    }
    return iter;
}

static map_bool_t map_iter_node_matches(const map_base_t *m,
                                        const map_iter_t *iter,
                                        const map_node_t *node)
{
    map_lookup_t lookup = {0};
    if (!m || !iter || !node) {
        return MAP_FALSE;
    }
    if (!iter->key_filter) {
        return MAP_TRUE;
    }
    lookup.bytes = iter->query_key;
    lookup.match_key = iter->query_key;
    lookup.pointer_key = iter->query_pointer;
    if (m->func_key_size) {
        if (iter->ksize < 0) {
            return MAP_FALSE;
        }
        lookup.size = iter->query_size;
        lookup.bytes = iter->query_key;
        lookup.match_key = iter->query_key;
    } else {
        lookup.size = sizeof(void *);
        lookup.bytes = &lookup.pointer_key;
        lookup.match_key = lookup.bytes;
    }
    lookup.hash = iter->query_hash;
    return map_node_matches(m, node, &lookup);
}

map_bool_t map_next(map_base_t *m, map_iter_t *iter)
{
    map_node_t *candidate = (map_node_t *)0;
    if (!m || !iter || !m->buckets || m->nbuckets <= 0) {
        if (iter) {
            iter->current = NULL;
            iter->node = NULL;
        }
        return MAP_FALSE;
    }
    if (iter->owner != m) {
        iter->owner = m;
        iter->bucketIdx = -1;
        iter->next_node = NULL;
        iter->current = NULL;
        iter->key_filter = MAP_FALSE;
    }
    for (;;) {
        if (iter->next_node) {
            candidate = iter->next_node;
            iter->next_node = candidate->next;
        } else {
            if (iter->bucketIdx < 0) {
                iter->bucketIdx = 0;
            } else if (iter->bucketIdx < m->nbuckets) {
                ++iter->bucketIdx;
            }
            while (iter->bucketIdx < m->nbuckets &&
                   !m->buckets[iter->bucketIdx]) {
                ++iter->bucketIdx;
            }
            if (iter->bucketIdx >= m->nbuckets) {
                iter->current = NULL;
                iter->node = NULL;
                iter->value = NULL;
                return MAP_FALSE;
            }
            candidate = m->buckets[iter->bucketIdx];
            iter->next_node = candidate->next;
        }
        if (!map_iter_node_matches(m, iter, candidate)) {
            continue;
        }
        iter->current = candidate;
        iter->node = candidate;
        iter->key = map_node_key(candidate);
        if (!iter->key_filter) {
            iter->ksize = candidate->key_size > (size_t)MAP_MAX_INT
                ? MAP_MAX_INT : (int)candidate->key_size;
        }
        iter->value = map_node_value(candidate);
        return MAP_TRUE;
    }
}

map_bool_t map_erase(map_base_t *m, map_iter_t *iter)
{
    map_node_t **link = (map_node_t **)0;
    map_node_t *current = (map_node_t *)0;
    if (!m || !iter || !m->func_free || iter->owner != m || !iter->current ||
        !m->buckets || iter->bucketIdx < 0 || iter->bucketIdx >= m->nbuckets) {
        return MAP_FALSE;
    }
    current = iter->current;
    link = &m->buckets[iter->bucketIdx];
    while (*link && *link != current) {
        link = &(*link)->next;
    }
    if (!*link) {
        iter->current = NULL;
        iter->node = NULL;
        return MAP_FALSE;
    }
    *link = current->next;
    if (iter->next_node == current) {
        iter->next_node = current->next;
    }
    if (m->func_free) {
        m->func_free(m->mem_pool, current);
    }
    if (m->nnodes > 0) {
        --m->nnodes;
    }
    iter->current = NULL;
    iter->node = NULL;
    iter->key = NULL;
    iter->value = NULL;
    /* Return whether another item is available; map_next performs the actual
     * advance, so `while (map_next(...)) map_erase(...)` visits every node. */
    return iter->next_node || iter->bucketIdx + 1 < m->nbuckets
        ? MAP_TRUE : MAP_FALSE;
}

static wchar_t map_wtolower(wchar_t value)
{
    if (value >= (wchar_t)L'A' && value <= (wchar_t)L'Z') {
        value = (wchar_t)(value + (wchar_t)(L'a' - L'A'));
    }
    return value;
}

unsigned int map_wcsihash(const void *key, size_t size)
{
    const wchar_t *text = (const wchar_t *)key;
    size_t units = size / sizeof(wchar_t);
    unsigned int hash = 5381U;
    if (!text) {
        return hash;
    }
    while (units != 0 && *text) {
        hash = ((hash << 5) + hash) ^ (unsigned int)map_wtolower(*text++);
        --units;
    }
    return hash;
}

int map_wcssize(const void *key)
{
    const wchar_t *text = (const wchar_t *)key;
    size_t count = 0;
    if (!text) {
        return 0;
    }
    while (*text++) {
        if (count == (size_t)MAP_MAX_INT) {
            return -1;
        }
        ++count;
    }
    ++count; /* Include the terminator, as the historical helper did. */
    if (count > (size_t)MAP_MAX_INT / sizeof(wchar_t)) {
        return -1;
    }
    return (int)(count * sizeof(wchar_t));
}

map_bool_t map_wcsimatch(const void *key1, const void *key2)
{
    const wchar_t *left = (const wchar_t *)key1;
    const wchar_t *right = (const wchar_t *)key2;
    if (!left || !right) {
        return left == right ? MAP_TRUE : MAP_FALSE;
    }
    while (*left && *right) {
        if (map_wtolower(*left++) != map_wtolower(*right++)) {
            return MAP_FALSE;
        }
    }
    return *left == *right ? MAP_TRUE : MAP_FALSE;
}

map_bool_t str_map_match(const void *key1, const void *key2)
{
    const wchar_t *const *left = (const wchar_t *const *)key1;
    const wchar_t *const *right = (const wchar_t *const *)key2;
    if (!left || !right) {
        return left == right ? MAP_TRUE : MAP_FALSE;
    }
    return map_wcsimatch(*left, *right);
}

unsigned int str_map_hash(const void *key, size_t size)
{
    const wchar_t *const *text = (const wchar_t *const *)key;
    const wchar_t *ptr = (const wchar_t *)0;
    unsigned int hash = 5381U;
    (void)size;
    if (!text || !*text) {
        return hash;
    }
    ptr = *text;
    while (*ptr) {
        hash = ((hash << 5) + hash) ^ (unsigned int)map_wtolower(*ptr);
        ++ptr;
    }
    return hash;
}
