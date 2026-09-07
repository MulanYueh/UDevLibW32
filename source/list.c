#include "list.h"

static LIST_ELEM *list_elem(void *elem)
{
    return (LIST_ELEM *)elem;
}

static const LIST_ELEM *list_const_elem(const void *elem)
{
    return (const LIST_ELEM *)elem;
}

int List_Init(LIST *list)
{
    if (!list) {
        return 0;
    }
    list->head = NULL;
    list->tail = NULL;
    list->count = 0;
    return 1;
}

void List_Clear(LIST *list)
{
    LIST_ELEM *current = NULL;
    LIST_ELEM *next = NULL;
    size_t remaining = 0;

    if (!list) {
        return;
    }
    current = list->head;
    remaining = list->count;
    while (current && remaining) {
        next = current->next;
        current->next = NULL;
        current->prev = NULL;
        current = next;
        --remaining;
    }
    list->head = NULL;
    list->tail = NULL;
    list->count = 0;
}

void *List_Head(const LIST *list)
{
    return list ? (void *)list->head : NULL;
}

void *List_Tail(const LIST *list)
{
    return list ? (void *)list->tail : NULL;
}

size_t List_Count(const LIST *list)
{
    return list ? list->count : 0;
}

void *List_Next(const void *elem)
{
    const LIST_ELEM *current = list_const_elem(elem);
    return current ? (void *)current->next : NULL;
}

void *List_Prev(const void *elem)
{
    const LIST_ELEM *current = list_const_elem(elem);
    return current ? (void *)current->prev : NULL;
}

int List_Contains(const LIST *list, const void *elem)
{
    const LIST_ELEM *current = NULL;
    size_t remaining = 0;

    if (!list || !elem) {
        return 0;
    }
    current = list->head;
    remaining = list->count;
    while (current && remaining) {
        if (current == list_const_elem(elem)) {
            return 1;
        }
        current = current->next;
        --remaining;
    }
    return 0;
}

static int list_can_insert(const LIST *list, const LIST_ELEM *oldElem,
                           const LIST_ELEM *newElem)
{
    if (!list || !newElem || oldElem == newElem) {
        return 0;
    }
    if (List_Contains(list, newElem)) {
        return 0;
    }
    if (oldElem && !List_Contains(list, oldElem)) {
        return 0;
    }
    /* SIZE_MAX is intentionally avoided for old C89-compatible headers. */
    if (list->count == (size_t)-1) {
        return 0;
    }
    return 1;
}

int List_Insert_Before(LIST *list, void *oldElemPtr, void *newElemPtr)
{
    LIST_ELEM *oldElem = list_elem(oldElemPtr);
    LIST_ELEM *newElem = list_elem(newElemPtr);
    LIST_ELEM *previous = NULL;

    if (!list_can_insert(list, oldElem, newElem)) {
        return 0;
    }
    if (!oldElem) {
        newElem->prev = NULL;
        newElem->next = list->head;
        if (list->head) {
            list->head->prev = newElem;
        } else {
            list->tail = newElem;
        }
        list->head = newElem;
    } else {
        previous = oldElem->prev;
        newElem->prev = previous;
        newElem->next = oldElem;
        oldElem->prev = newElem;
        if (previous) {
            previous->next = newElem;
        } else {
            list->head = newElem;
        }
    }
    ++list->count;
    return 1;
}

int List_Insert_After(LIST *list, void *oldElemPtr, void *newElemPtr)
{
    LIST_ELEM *oldElem = list_elem(oldElemPtr);
    LIST_ELEM *newElem = list_elem(newElemPtr);
    LIST_ELEM *next = NULL;

    if (!list_can_insert(list, oldElem, newElem)) {
        return 0;
    }
    if (!oldElem) {
        newElem->prev = list->tail;
        newElem->next = NULL;
        if (list->tail) {
            list->tail->next = newElem;
        } else {
            list->head = newElem;
        }
        list->tail = newElem;
    } else {
        next = oldElem->next;
        newElem->prev = oldElem;
        newElem->next = next;
        oldElem->next = newElem;
        if (next) {
            next->prev = newElem;
        } else {
            list->tail = newElem;
        }
    }
    ++list->count;
    return 1;
}

int List_Remove(LIST *list, void *elemPtr)
{
    LIST_ELEM *elem = list_elem(elemPtr);

    if (!list || !elem || !List_Contains(list, elem) || !list->count) {
        return 0;
    }
    if (elem->prev) {
        elem->prev->next = elem->next;
    } else {
        list->head = elem->next;
    }
    if (elem->next) {
        elem->next->prev = elem->prev;
    } else {
        list->tail = elem->prev;
    }
    elem->next = NULL;
    elem->prev = NULL;
    --list->count;
    return 1;
}
