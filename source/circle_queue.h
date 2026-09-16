#ifndef CIRCLE_QUEUE_H_INCLUDED
#define CIRCLE_QUEUE_H_INCLUDED

/* The project supplies stdint2.h for freestanding/MSVC builds.  Hosted GCC
 * and Clang already provide a compatible stdint.h; using it there avoids the
 * two libraries' different int_fast16_t definitions colliding in C++. */
#if defined(CIRCLE_QUEUE_USE_STDINT2) || \
    (defined(_MSC_VER) && !defined(__clang__) && !defined(__GNUC__))
#define CIRCLE_QUEUE_USING_STDINT2 1
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#include "stdint2.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#else
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t circle_queue_bool_t;

#define CIRCLE_QUEUE_FALSE ((circle_queue_bool_t)0u)
#define CIRCLE_QUEUE_TRUE  ((circle_queue_bool_t)1u)

/* Kept for source compatibility with code that used the old constants. */
#define CIRCLE_QUEUE_ITEM_DATA_SIZE 0x400u
#define CIRCLE_QUEUE_ITEM_SIZE      0x100u

/*
 * A queue item is borrowed by the queue.  Push stores the pointer and length;
 * it does not copy or release the pointed-to buffer.  The caller owns that
 * buffer until Pop returns it or until the queue is destroyed.
 */
typedef struct _CIRCLE_QUEUE_ITEM {
    uint32_t length;
    uint8_t *lpData;
} CIRCLE_QUEUE_ITEM, *PCIRCLE_QUEUE_ITEM;

/*
 * The queue has one producer/consumer state machine and is not synchronized.
 * Callers must serialize concurrent operations.  item_cnt is the capacity;
 * count is the number of live entries in [start_offset, end_offset).
 */
typedef struct _CIRCLE_QUEUE_CONTEXT {
    PCIRCLE_QUEUE_ITEM ptrItems;
    uint32_t item_cnt;
    uint32_t start_offset;
    uint32_t end_offset;
    uint32_t count;
    circle_queue_bool_t inited;
} CIRCLE_QUEUE_CONTEXT, *PCIRCLE_QUEUE_CONTEXT;

/*
 * Create allocates maxCnt item slots.  Push has overwrite-oldest semantics:
 * when the queue is full, the new item replaces the oldest item.  Queue item
 * data is never allocated, copied, or freed by this module.
 */
circle_queue_bool_t Interface_CircleQueue_Create(
    PCIRCLE_QUEUE_CONTEXT lpCirBufCtx,
    uint32_t maxCnt);

circle_queue_bool_t Interface_CircleQueue_Destroy(
    PCIRCLE_QUEUE_CONTEXT lpCirBufCtx);

circle_queue_bool_t Interface_CircleQueue_Push(
    PCIRCLE_QUEUE_CONTEXT lpCirBufCtx,
    const CIRCLE_QUEUE_ITEM *lpItem);

circle_queue_bool_t Interface_CircleQueue_Pop(
    PCIRCLE_QUEUE_CONTEXT lpCirBufCtx,
    PCIRCLE_QUEUE_ITEM lpItem);

circle_queue_bool_t Interface_CircleQueue_IsEmpty(
    const CIRCLE_QUEUE_CONTEXT *lpCirBufCtx);

/* These queries make the explicit state useful without inspecting internals. */
uint32_t Interface_CircleQueue_Count(
    const CIRCLE_QUEUE_CONTEXT *lpCirBufCtx);

uint32_t Interface_CircleQueue_Capacity(
    const CIRCLE_QUEUE_CONTEXT *lpCirBufCtx);

#ifdef __cplusplus
}
#endif

#endif /* CIRCLE_QUEUE_H_INCLUDED */
