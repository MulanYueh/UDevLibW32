/*
 * User-mode circle queue demo.
 *
 * The queue owns only its descriptor array.  Payload buffers are borrowed:
 * this demo allocates them with Allocator_Malloc, pushes their addresses, and
 * releases them after Pop returns them.  The overwrite example intentionally
 * uses stack buffers so no dropped payload needs to be reclaimed.
 *
 * Build the C sources together with this file:
 *
 *   gcc -std=c11 -Wall -Wextra -Wconversion -Wshadow -Werror -I. \
 *       circle_queue_demo.c circle_queue.c allocator.c libc.c \
 *       -o circle_queue_demo.exe
 */

#include "allocator.h"
#include "circle_queue.h"
#include "libc.h"

#include <stdio.h>

static void demo_release_item(CIRCLE_QUEUE_ITEM *item)
{
    if (item == (PCIRCLE_QUEUE_ITEM)0) {
        return;
    }

    Allocator_Free(item->lpData);
    item->lpData = (uint8_t *)0;
    item->length = 0U;
}

static int demo_push_text(CIRCLE_QUEUE_CONTEXT *queue, const char *text)
{
    CIRCLE_QUEUE_ITEM item;
    size_t text_length;
    uint8_t *buffer;

    if (queue == (PCIRCLE_QUEUE_CONTEXT)0 || text == (const char *)0) {
        return 0;
    }

    text_length = libc_strlen(text);
    if (text_length == 0U || text_length >= (size_t)UINT32_MAX) {
        return 0;
    }

    buffer = (uint8_t *)Allocator_Malloc(text_length + 1U);
    if (buffer == (uint8_t *)0) {
        return 0;
    }

    (void)libc_memcpy(buffer, text, text_length + 1U);
    item.length = (uint32_t)text_length;
    item.lpData = buffer;

    if (Interface_CircleQueue_Push(queue, &item) == CIRCLE_QUEUE_FALSE) {
        Allocator_Free(buffer);
        return 0;
    }

    return 1;
}

static int demo_drain_owned(CIRCLE_QUEUE_CONTEXT *queue)
{
    CIRCLE_QUEUE_ITEM item;

    if (queue == (PCIRCLE_QUEUE_CONTEXT)0) {
        return 0;
    }

    while (Interface_CircleQueue_Pop(queue, &item) == CIRCLE_QUEUE_TRUE) {
        printf("  pop: %.*s (length=%u, data=%p)\n",
               (int)item.length, (const char *)item.lpData,
               (unsigned int)item.length, (void *)item.lpData);
        demo_release_item(&item);
    }

    return Interface_CircleQueue_IsEmpty(queue) == CIRCLE_QUEUE_TRUE;
}

static int demo_overwrite_oldest(CIRCLE_QUEUE_CONTEXT *queue)
{
    static uint8_t first[] = "first";
    static uint8_t second[] = "second";
    static uint8_t third[] = "third";
    static uint8_t newest[] = "newest";
    CIRCLE_QUEUE_ITEM item;
    CIRCLE_QUEUE_ITEM output;
    const CIRCLE_QUEUE_ITEM expected[] = {
        {6U, second}, {5U, third}, {6U, newest}};
    size_t index;

    item.length = 5U;
    item.lpData = first;
    if (Interface_CircleQueue_Push(queue, &item) == CIRCLE_QUEUE_FALSE) {
        return 0;
    }
    item.length = 6U;
    item.lpData = second;
    if (Interface_CircleQueue_Push(queue, &item) == CIRCLE_QUEUE_FALSE) {
        return 0;
    }
    item.length = 5U;
    item.lpData = third;
    if (Interface_CircleQueue_Push(queue, &item) == CIRCLE_QUEUE_FALSE) {
        return 0;
    }

    /* Capacity is three, so this push replaces "first" with "newest". */
    item.length = 6U;
    item.lpData = newest;
    if (Interface_CircleQueue_Push(queue, &item) == CIRCLE_QUEUE_FALSE ||
        Interface_CircleQueue_Count(queue) != 3U) {
        return 0;
    }

    index = 0U;
    while (index < 3U) {
        if (Interface_CircleQueue_Pop(queue, &output) == CIRCLE_QUEUE_FALSE ||
            output.length != expected[index].length ||
            output.lpData != expected[index].lpData) {
            return 0;
        }
        printf("  overwrite pop: %.*s\n", (int)output.length,
               (const char *)output.lpData);
        ++index;
    }

    return Interface_CircleQueue_IsEmpty(queue) == CIRCLE_QUEUE_TRUE;
}

int main(void)
{
    CIRCLE_QUEUE_CONTEXT queue = {
        (PCIRCLE_QUEUE_ITEM)0, 0U, 0U, 0U, 0U, CIRCLE_QUEUE_FALSE};
    int success = 0;

    if (Interface_CircleQueue_Create(&queue, 3U) == CIRCLE_QUEUE_FALSE) {
        fputs("Interface_CircleQueue_Create failed\n", stderr);
        return 1;
    }

    printf("queue created: capacity=%u, count=%u\n",
           (unsigned int)Interface_CircleQueue_Capacity(&queue),
           (unsigned int)Interface_CircleQueue_Count(&queue));

    /* Payload memory is allocated by the caller, not by the queue. */
    if (!demo_push_text(&queue, "alpha") ||
        !demo_push_text(&queue, "bravo") ||
        !demo_push_text(&queue, "charlie")) {
        fputs("payload allocation or push failed\n", stderr);
        (void)demo_drain_owned(&queue);
        (void)Interface_CircleQueue_Destroy(&queue);
        return 1;
    }

    puts("FIFO payloads:");
    if (!demo_drain_owned(&queue)) {
        fputs("FIFO drain failed\n", stderr);
        (void)Interface_CircleQueue_Destroy(&queue);
        return 1;
    }

    puts("overwrite-oldest payloads:");
    if (!demo_overwrite_oldest(&queue)) {
        fputs("overwrite demonstration failed\n", stderr);
        (void)Interface_CircleQueue_Destroy(&queue);
        return 1;
    }

    success = Interface_CircleQueue_Destroy(&queue) == CIRCLE_QUEUE_TRUE;
    printf("queue destroyed: %s\n", success ? "yes" : "no");
    return success ? 0 : 1;
}
