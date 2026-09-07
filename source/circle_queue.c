#include "circle_queue.h"
#include "def.h"
#include "allocator.h"

/* Link this translation unit with allocator.c and the project's libc.c. */

#include <stddef.h>

/* Use the project's CRT-independent memory primitive.  The explicit
 * declaration is only needed for an opt-in stdint2 freestanding build where
 * including libc.h would reintroduce the platform stdint typedef collision. */
#if !defined(CIRCLE_QUEUE_USING_STDINT2)
#include "libc.h"
#define CIRCLE_QUEUE_LIBC_DECLARED 1
#endif

#ifndef CIRCLE_QUEUE_LIBC_DECLARED
#if defined(_MSC_VER)
#define CIRCLE_QUEUE_LIBC_CALL __cdecl
#elif defined(__i386__) && defined(_WIN32) && \
      (defined(__GNUC__) || defined(__clang__))
#define CIRCLE_QUEUE_LIBC_CALL __attribute__((__cdecl__))
#else
#define CIRCLE_QUEUE_LIBC_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif
extern void *CIRCLE_QUEUE_LIBC_CALL libc_memset(void *dst, int value,
                                                 size_t count);
#ifdef __cplusplus
} /* extern "C" */
#endif
#endif

static uint32_t
circle_queue_next_index(uint32_t index, uint32_t capacity)
{
    /* The branch avoids a division and is safe for the largest uint32_t
     * capacity because every valid index is strictly less than capacity. */
    return (index + 1u == capacity) ? 0u : index + 1u;
}

static circle_queue_bool_t
circle_queue_is_valid(const CIRCLE_QUEUE_CONTEXT *context)
{
    return (context != (const CIRCLE_QUEUE_CONTEXT *)0 &&
            context->inited != CIRCLE_QUEUE_FALSE &&
            context->ptrItems != (PCIRCLE_QUEUE_ITEM)0 &&
            context->item_cnt != 0u &&
            context->start_offset < context->item_cnt &&
            context->end_offset < context->item_cnt &&
            context->count <= context->item_cnt)
               ? CIRCLE_QUEUE_TRUE
               : CIRCLE_QUEUE_FALSE;
}

circle_queue_bool_t
Interface_CircleQueue_Create(PCIRCLE_QUEUE_CONTEXT context, uint32_t maxCnt)
{
    size_t allocation_size = 0;

    if (context == (PCIRCLE_QUEUE_CONTEXT)0 || maxCnt == 0u) {
        return CIRCLE_QUEUE_FALSE;
    }

    /* Create must not leak an existing allocation.  A caller should pass a
     * zero-initialized context for the first creation. */
    if (context->inited != CIRCLE_QUEUE_FALSE ||
        context->ptrItems != (PCIRCLE_QUEUE_ITEM)0) {
        return CIRCLE_QUEUE_FALSE;
    }

    allocation_size = (size_t)maxCnt;
    if (allocation_size > ((size_t)-1) / sizeof(CIRCLE_QUEUE_ITEM)) {
        return CIRCLE_QUEUE_FALSE;
    }
    allocation_size *= sizeof(CIRCLE_QUEUE_ITEM);

    context->ptrItems = (PCIRCLE_QUEUE_ITEM)0;
    context->item_cnt = 0u;
    context->start_offset = 0u;
    context->end_offset = 0u;
    context->count = 0u;

#if defined(_KERNEL_MODE)
    context->ptrItems = (PCIRCLE_QUEUE_ITEM)Allocator_Malloc(
        CIRCLE_QUEUE_TRUE, allocation_size,
        (ULONG)CIRCLE_QUEUE_POOL_TAG);
#else
    context->ptrItems = (PCIRCLE_QUEUE_ITEM)Allocator_Malloc(allocation_size);
#endif

    if (context->ptrItems == (PCIRCLE_QUEUE_ITEM)0) {
        return CIRCLE_QUEUE_FALSE;
    }

    (void)libc_memset(context->ptrItems, 0, allocation_size);
    context->item_cnt = maxCnt;
    context->inited = CIRCLE_QUEUE_TRUE;
    return CIRCLE_QUEUE_TRUE;
}

circle_queue_bool_t
Interface_CircleQueue_Destroy(PCIRCLE_QUEUE_CONTEXT context)
{
    if (context == (PCIRCLE_QUEUE_CONTEXT)0 ||
        context->inited == CIRCLE_QUEUE_FALSE) {
        return CIRCLE_QUEUE_FALSE;
    }

#if defined(_KERNEL_MODE)
    Allocator_Free(context->ptrItems, (ULONG)CIRCLE_QUEUE_POOL_TAG);
#else
    Allocator_Free(context->ptrItems);
#endif

    context->ptrItems = (PCIRCLE_QUEUE_ITEM)0;
    context->item_cnt = 0u;
    context->start_offset = 0u;
    context->end_offset = 0u;
    context->count = 0u;
    context->inited = CIRCLE_QUEUE_FALSE;
    return CIRCLE_QUEUE_TRUE;
}

circle_queue_bool_t
Interface_CircleQueue_Push(PCIRCLE_QUEUE_CONTEXT context,
                           const CIRCLE_QUEUE_ITEM *item)
{
    PCIRCLE_QUEUE_ITEM slot = (PCIRCLE_QUEUE_ITEM)0;

    if (circle_queue_is_valid(context) == CIRCLE_QUEUE_FALSE ||
        item == (const CIRCLE_QUEUE_ITEM *)0 || item->length == 0u ||
        item->lpData == (uint8_t *)0) {
        return CIRCLE_QUEUE_FALSE;
    }

    slot = &context->ptrItems[context->end_offset];
    *slot = *item;

    if (context->count == context->item_cnt) {
        /* Full: end points at the oldest entry, so advance start after the
         * replacement and keep count equal to capacity. */
        context->start_offset =
            circle_queue_next_index(context->start_offset, context->item_cnt);
    } else {
        ++context->count;
    }

    context->end_offset =
        circle_queue_next_index(context->end_offset, context->item_cnt);
    return CIRCLE_QUEUE_TRUE;
}

circle_queue_bool_t
Interface_CircleQueue_Pop(PCIRCLE_QUEUE_CONTEXT context, PCIRCLE_QUEUE_ITEM item)
{
    PCIRCLE_QUEUE_ITEM slot = (PCIRCLE_QUEUE_ITEM)0;
    CIRCLE_QUEUE_ITEM value = {0};

    if (circle_queue_is_valid(context) == CIRCLE_QUEUE_FALSE ||
        context->count == 0u) {
        return CIRCLE_QUEUE_FALSE;
    }

    slot = &context->ptrItems[context->start_offset];
    /* Copy first so an output item that aliases a queue slot is still valid. */
    value = *slot;
    slot->length = 0u;
    slot->lpData = (uint8_t *)0;
    if (item != (PCIRCLE_QUEUE_ITEM)0) {
        *item = value;
    }
    context->start_offset =
        circle_queue_next_index(context->start_offset, context->item_cnt);
    --context->count;
    return CIRCLE_QUEUE_TRUE;
}

circle_queue_bool_t
Interface_CircleQueue_IsEmpty(const CIRCLE_QUEUE_CONTEXT *context)
{
    return (circle_queue_is_valid(context) == CIRCLE_QUEUE_FALSE ||
            context->count == 0u)
               ? CIRCLE_QUEUE_TRUE
               : CIRCLE_QUEUE_FALSE;
}

uint32_t
Interface_CircleQueue_Count(const CIRCLE_QUEUE_CONTEXT *context)
{
    return circle_queue_is_valid(context) == CIRCLE_QUEUE_FALSE ? 0u
                                                                  : context->count;
}

uint32_t
Interface_CircleQueue_Capacity(const CIRCLE_QUEUE_CONTEXT *context)
{
    return circle_queue_is_valid(context) == CIRCLE_QUEUE_FALSE ? 0u
                                                                  : context->item_cnt;
}
