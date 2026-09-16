/*
 * Small, intrusive, doubly-linked list.
 *
 * Nodes are embedded in the object owned by the caller.  The list never
 * allocates or frees node memory, which makes the same implementation usable
 * in user mode and in a Windows kernel.  A node may belong to at most one
 * list at a time; callers must remove a node before reusing it. The node
 * layout intentionally remains the historical two-pointer shape so existing
 * pool/page structures keep their ABI.
 */
#ifndef _LIST_H_INCLUDED
#define _LIST_H_INCLUDED

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LIST LIST;

typedef struct LIST_ELEM {
    struct LIST_ELEM *next;
    struct LIST_ELEM *prev;
} LIST_ELEM;

struct LIST {
    LIST_ELEM *head;
    LIST_ELEM *tail;
    size_t count;
};

/* All mutating functions return non-zero on success and zero on bad input. */
int List_Init(LIST *list);
void List_Clear(LIST *list);

void *List_Head(const LIST *list);
void *List_Tail(const LIST *list);
size_t List_Count(const LIST *list);
void *List_Next(const void *elem);
void *List_Prev(const void *elem);
int List_Contains(const LIST *list, const void *elem);

/* Passing a NULL old element inserts at the head/tail respectively. */
int List_Insert_Before(LIST *list, void *oldElem, void *newElem);
int List_Insert_After(LIST *list, void *oldElem, void *newElem);
int List_Remove(LIST *list, void *elem);

#ifdef __cplusplus
}
#endif

#endif /* _LIST_H_INCLUDED */
