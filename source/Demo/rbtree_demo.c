/*
 * Complete user-mode demonstration of the intrusive red-black tree.
 *
 * Build (MinGW):
 *
 *   gcc -std=c11 -Wall -Wextra -Wconversion -Wshadow -Werror -I. \
 *       rbtree_demo.c rbtree.c -o rbtree_demo.exe
 *
 * The tree never allocates, copies, or frees user records.  Each record puts
 * rbnode_t first, points node.key at its own key, and remains caller-owned
 * until the caller deletes it and releases the containing record.
 */

#include "rbtree.h"

#include <stdio.h>
#include <stdlib.h>

#define DEMO_RECORD_COUNT 8U

typedef struct demo_record_t {
    rbnode_t node;
    int key;
    const char *label;
} demo_record_t;

static int demo_compare_int(const void *left, const void *right)
{
    const int first = *(const int *)left;
    const int second = *(const int *)right;

    return first < second ? -1 : (first > second ? 1 : 0);
}

static int demo_check(int condition, const char *message)
{
    if (condition == 0) {
        fprintf(stderr, "rbtree demo error: %s\n", message);
        return 0;
    }
    return 1;
}

static void demo_record_init(demo_record_t *record, int key,
                             const char *label)
{
    record->node.parent = (rbnode_t *)0;
    record->node.left = (rbnode_t *)0;
    record->node.right = (rbnode_t *)0;
    record->node.key = &record->key;
    record->node.color = 0U;
    record->key = key;
    record->label = label;
}

static demo_record_t *demo_record_from_node(rbnode_t *node)
{
    if (node == (rbnode_t *)0 || node == RBTREE_NULL) {
        return (demo_record_t *)0;
    }
    /* rbnode_t is the first member, so this cast recovers the record. */
    return (demo_record_t *)node;
}

static void demo_print_record(const rbnode_t *node)
{
    const demo_record_t *record = (const demo_record_t *)node;

    printf("  key=%d, label=%s\n", record->key, record->label);
}

static void demo_print_forward(rbtree_t *tree)
{
    rbnode_t *node;

    node = rbtree_first(tree);
    while (node != RBTREE_NULL) {
        demo_print_record(node);
        node = rbtree_next(node);
    }
}

static void demo_print_reverse(rbtree_t *tree)
{
    rbnode_t *node;

    node = rbtree_last(tree);
    while (node != RBTREE_NULL) {
        demo_print_record(node);
        node = rbtree_previous(node);
    }
}

static void demo_postorder_callback(rbnode_t *node, void *argument)
{
    size_t *visited = (size_t *)argument;

    printf("  postorder visit: key=%d\n", *(const int *)node->key);
    if (visited != (size_t *)0) {
        ++*visited;
    }
}

static void demo_forget_record(demo_record_t **records, size_t record_count,
                               demo_record_t *record)
{
    size_t index;

    index = 0U;
    while (index < record_count) {
        if (records[index] == record) {
            records[index] = (demo_record_t *)0;
            return;
        }
        ++index;
    }
}

static int demo_delete_and_release(rbtree_t *tree,
                                   demo_record_t **records,
                                   size_t record_count,
                                   int key)
{
    rbnode_t *removed;
    demo_record_t *record;
    int detached;

    removed = rbtree_delete(tree, &key);
    if (removed == (rbnode_t *)0) {
        fprintf(stderr, "rbtree demo error: key %d was not deleted\n", key);
        return 0;
    }

    record = demo_record_from_node(removed);
    if (record == (demo_record_t *)0) {
        fprintf(stderr, "rbtree demo error: delete returned an invalid node\n");
        return 0;
    }
    detached = removed->parent == RBTREE_NULL &&
               removed->left == RBTREE_NULL &&
               removed->right == RBTREE_NULL;
    printf("deleted key=%d (%s), detached=%s; caller now frees the record\n",
           record->key, record->label, detached ? "yes" : "no");
    demo_forget_record(records, record_count, record);
    free(record);
    return detached;
}

static void demo_release_tree(rbtree_t *tree, demo_record_t **records,
                              size_t record_count)
{
    rbnode_t *node;
    rbnode_t *removed;
    demo_record_t *record;
    size_t index;

    /* Delete before free: the key is embedded in the still-live record. */
    while (tree->count != 0U) {
        node = rbtree_first(tree);
        record = demo_record_from_node(node);
        if (record == (demo_record_t *)0) {
            fprintf(stderr, "rbtree demo error: cleanup found no first node\n");
            break;
        }
        removed = rbtree_delete(tree, &record->key);
        if (removed == (rbnode_t *)0) {
            fprintf(stderr, "rbtree demo error: cleanup deletion failed\n");
            break;
        }
        demo_forget_record(records, record_count, record);
        free(record);
    }

    /* Also release a record allocated just before a failed insertion. */
    index = 0U;
    while (index < record_count) {
        if (records[index] != (demo_record_t *)0) {
            free(records[index]);
            records[index] = (demo_record_t *)0;
        }
        ++index;
    }
}

int main(void)
{
    rbtree_t tree;
    demo_record_t *records[DEMO_RECORD_COUNT] = {0};
    const int keys[DEMO_RECORD_COUNT] = {40, 20, 60, 10, 30, 50, 70, 25};
    const char *labels[DEMO_RECORD_COUNT] = {
        "root", "engineering", "sales", "intern", "platform",
        "support", "research", "release"};
    demo_record_t duplicate;
    demo_record_t *record;
    rbnode_t *node;
    size_t allocated;
    size_t index;
    size_t before;
    size_t postorder_count;
    int query;
    int exact;
    int status;

    allocated = 0U;
    status = 1;
    rbtree_init(&tree, demo_compare_int);

    printf("=== intrusive red-black tree user-mode demo ===\n");
    printf("tree initialized: count=%zu, empty=%s\n", tree.count,
           rbtree_first(&tree) == RBTREE_NULL ? "yes" : "no");
    puts("\nInserting caller-owned records:");

    index = 0U;
    while (index < DEMO_RECORD_COUNT) {
        records[index] = (demo_record_t *)malloc(sizeof(*records[index]));
        if (records[index] == (demo_record_t *)0) {
            fprintf(stderr, "rbtree demo error: malloc failed\n");
            goto cleanup;
        }
        ++allocated;
        demo_record_init(records[index], keys[index], labels[index]);
        node = rbtree_insert(&tree, &records[index]->node);
        if (node != &records[index]->node) {
            fprintf(stderr, "rbtree demo error: insert failed for key %d\n",
                    keys[index]);
            goto cleanup;
        }
        printf("  inserted key=%d (%s), count=%zu\n", keys[index],
               labels[index], tree.count);
        ++index;
    }

    /* Equal keys are rejected, and the rejected record remains caller-owned. */
    demo_record_init(&duplicate, 30, "duplicate-key");
    node = rbtree_insert(&tree, &duplicate.node);
    if (!demo_check(node == (rbnode_t *)0 && tree.count == DEMO_RECORD_COUNT,
                    "duplicate key was unexpectedly inserted")) {
        goto cleanup;
    }
    printf("  duplicate key=30 rejected; count remains %zu\n", tree.count);

    puts("\nExact search:");
    query = 30;
    node = rbtree_search(&tree, &query);
    if (!demo_check(node != (rbnode_t *)0 && *(const int *)node->key == query,
                    "exact search did not find key 30")) {
        goto cleanup;
    }
    record = demo_record_from_node(node);
    printf("  search key=%d -> %s\n", query, record->label);

    query = 35;
    if (!demo_check(rbtree_search(&tree, &query) == (rbnode_t *)0,
                    "missing exact search unexpectedly succeeded")) {
        goto cleanup;
    }
    printf("  search key=%d -> not found\n", query);

    puts("\nPredecessor search (rbtree_find_less_equal):");
    query = 34;
    node = (rbnode_t *)0;
    exact = rbtree_find_less_equal(&tree, &query, &node);
    if (!demo_check(exact == 0 && node != (rbnode_t *)0 &&
                        *(const int *)node->key == 30,
                    "predecessor search returned the wrong node")) {
        goto cleanup;
    }
    printf("  key=%d -> predecessor key=%d (exact=%d)\n", query,
           *(const int *)node->key, exact);

    query = 5;
    node = (rbnode_t *)0;
    exact = rbtree_find_less_equal(&tree, &query, &node);
    if (!demo_check(exact == 0 && node == (rbnode_t *)0,
                    "predecessor search below minimum should be empty")) {
        goto cleanup;
    }
    printf("  key=%d -> no predecessor (exact=%d)\n", query, exact);

    query = 50;
    node = (rbnode_t *)0;
    exact = rbtree_find_less_equal(&tree, &query, &node);
    if (!demo_check(exact != 0 && node != (rbnode_t *)0 &&
                        *(const int *)node->key == query,
                    "predecessor search did not report an exact match")) {
        goto cleanup;
    }
    printf("  key=%d -> exact key=%d (exact=%d)\n", query,
           *(const int *)node->key, exact);

    puts("\nSorted traversal using first/next:");
    demo_print_forward(&tree);

    puts("\nSorted traversal using RBTREE_FOR:");
    record = (demo_record_t *)0;
    RBTREE_FOR(record, demo_record_t *, &tree) {
        printf("  key=%d, label=%s\n", record->key, record->label);
    }

    puts("\nReverse traversal using last/previous:");
    demo_print_reverse(&tree);

    puts("\nDeleting nodes:");
    if (!demo_delete_and_release(&tree, records, DEMO_RECORD_COUNT, 20)) {
        goto cleanup;
    }
    printf("  count after deleting 20: %zu\n", tree.count);

    query = 35;
    before = tree.count;
    node = rbtree_delete(&tree, &query);
    if (!demo_check(node == (rbnode_t *)0 && tree.count == before,
                    "deleting a missing key changed the tree")) {
        goto cleanup;
    }
    printf("  delete key=%d -> not found; count remains %zu\n", query,
           tree.count);

    /* Key 40 is the original root and has two children. */
    if (!demo_delete_and_release(&tree, records, DEMO_RECORD_COUNT, 40)) {
        goto cleanup;
    }
    printf("  count after deleting root 40: %zu\n", tree.count);

    puts("\nPostorder traversal (children are visited before parents):");
    postorder_count = 0U;
    traverse_postorder(&tree, demo_postorder_callback, &postorder_count);
    if (!demo_check(postorder_count == tree.count,
                    "postorder callback count does not match tree count")) {
        goto cleanup;
    }

    puts("\nRemaining records in sorted order:");
    demo_print_forward(&tree);

    status = 0;

cleanup:
    demo_release_tree(&tree, records, DEMO_RECORD_COUNT);
    printf("\ncleanup: caller released %zu allocated record(s); tree count=%zu, "
           "empty=%s\n", allocated, tree.count,
           tree.root == RBTREE_NULL ? "yes" : "no");
    if (tree.count != 0U || tree.root != RBTREE_NULL) {
        status = 1;
    }
    return status;
}
