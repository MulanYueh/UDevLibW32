/*
 * Independent Catch2 coverage for the intrusive red-black tree.
 *
 * Build the implementation as C and this file as C++:
 *
 *   gcc -std=c11 -I. -c rbtree.c
 *   g++ -std=c++17 -I. rbtree_unit_test.cpp rbtree.o -o rbtree_unit_test.exe
 */

#define CATCH_CONFIG_MAIN
#include "catch2/catch.hpp"

#include "rbtree.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

namespace
{

struct TestItem
{
    rbnode_t node;
    int key;
};

int compare_int(const void* left, const void* right)
{
    const int first = *static_cast<const int*>(left);
    const int second = *static_cast<const int*>(right);
    return first < second ? -1 : (first > second ? 1 : 0);
}

void prepare_item(TestItem& item, int key)
{
    item.node.parent = static_cast<rbnode_t*>(0);
    item.node.left = static_cast<rbnode_t*>(0);
    item.node.right = static_cast<rbnode_t*>(0);
    item.key = key;
    item.node.key = &item.key;
    item.node.color = 0U;
}

int item_key(const rbnode_t* node)
{
    return *static_cast<const int*>(node->key);
}

struct ValidationResult
{
    bool valid;
    std::size_t node_count;
    int black_height;
};

ValidationResult invalid_result()
{
    return ValidationResult{false, 0U, 0};
}

/* Check ordering, links, colors, and equal black height recursively. */
ValidationResult validate_subtree(const rbnode_t* node,
                                  const rbnode_t* expected_parent,
                                  bool has_lower,
                                  int lower,
                                  bool has_upper,
                                  int upper,
                                  std::size_t depth,
                                  std::size_t depth_limit)
{
    ValidationResult left;
    ValidationResult right;

    if (node == RBTREE_NULL) {
        return ValidationResult{true, 0U, 1};
    }
    if (node == static_cast<const rbnode_t*>(0) || depth > depth_limit ||
        node->parent != expected_parent || node->key == static_cast<const void*>(0) ||
        (node->color != 0U && node->color != 1U)) {
        return invalid_result();
    }

    const int node_value = item_key(node);
    if ((has_lower && node_value <= lower) ||
        (has_upper && node_value >= upper)) {
        return invalid_result();
    }
    if (node->color == 1U &&
        (node->left == static_cast<const rbnode_t*>(0) ||
         node->right == static_cast<const rbnode_t*>(0) ||
         node->left->color != 0U || node->right->color != 0U)) {
        return invalid_result();
    }

    left = validate_subtree(node->left, node, has_lower, lower, true,
                            node_value, depth + 1U, depth_limit);
    right = validate_subtree(node->right, node, true, node_value, has_upper,
                             upper, depth + 1U, depth_limit);
    if (!left.valid || !right.valid || left.black_height != right.black_height) {
        return invalid_result();
    }

    return ValidationResult{
        true,
        left.node_count + right.node_count + 1U,
        left.black_height + (node->color == 0U ? 1 : 0)};
}

bool validate_tree(const rbtree_t& tree, std::size_t expected_count)
{
    ValidationResult result;
    rbnode_t* sentinel = RBTREE_NULL;

    if (tree.root == static_cast<rbnode_t*>(0) || tree.count != expected_count ||
        tree.cmp == 0 || sentinel->parent != sentinel ||
        sentinel->left != sentinel || sentinel->right != sentinel ||
        sentinel->color != 0U) {
        return false;
    }
    if (tree.root == RBTREE_NULL) {
        return expected_count == 0U;
    }
    if (tree.root->parent != RBTREE_NULL || tree.root->color != 0U) {
        return false;
    }

    result = validate_subtree(tree.root, RBTREE_NULL, false, 0, false, 0,
                              0U, expected_count + 1U);
    return result.valid && result.node_count == expected_count;
}

std::vector<int> inorder_keys(rbtree_t* tree)
{
    std::vector<int> keys;
    rbnode_t* node = rbtree_first(tree);

    while (node != RBTREE_NULL) {
        keys.push_back(item_key(node));
        node = rbtree_next(node);
    }
    return keys;
}

void collect_postorder(rbnode_t* node, void* argument)
{
    std::vector<rbnode_t*>* visited = static_cast<std::vector<rbnode_t*>*>(argument);
    visited->push_back(node);
}

std::size_t find_node_index(const std::vector<rbnode_t*>& nodes,
                            const rbnode_t* wanted)
{
    for (std::size_t index = 0U; index < nodes.size(); ++index) {
        if (nodes[index] == wanted) {
            return index;
        }
    }
    return nodes.size();
}

} /* namespace */

TEST_CASE("rbtree initialization and invalid arguments are harmless",
          "[rbtree][validation]")
{
    rbtree_t tree = {};
    TestItem item = {};
    int key = 10;
    rbnode_t* result = &item.node;
    std::size_t callback_count = 0U;

    prepare_item(item, key);
    rbtree_init(static_cast<rbtree_t*>(0), compare_int);
    rbtree_init(&tree, compare_int);

    CHECK(tree.root == RBTREE_NULL);
    CHECK(tree.count == 0U);
    CHECK(tree.cmp == compare_int);
    CHECK(rbtree_insert(static_cast<rbtree_t*>(0), &item.node) == nullptr);
    CHECK(rbtree_insert(&tree, static_cast<rbnode_t*>(0)) == nullptr);
    CHECK(rbtree_insert(&tree, RBTREE_NULL) == nullptr);
    CHECK(rbtree_search(static_cast<rbtree_t*>(0), &key) == nullptr);
    CHECK(rbtree_delete(static_cast<rbtree_t*>(0), &key) == nullptr);
    CHECK(rbtree_find_less_equal(static_cast<rbtree_t*>(0), &key, &result) == 0);
    CHECK(result == nullptr);
    result = &item.node;
    CHECK(rbtree_find_less_equal(&tree, &key, static_cast<rbnode_t**>(0)) == 0);
    CHECK(rbtree_find_less_equal(&tree, &key, &result) == 0);
    CHECK(result == nullptr);
    CHECK(rbtree_first(&tree) == RBTREE_NULL);
    CHECK(rbtree_last(&tree) == RBTREE_NULL);
    CHECK(rbtree_next(static_cast<rbnode_t*>(0)) == nullptr);
    CHECK(rbtree_previous(static_cast<rbnode_t*>(0)) == nullptr);
    CHECK(rbtree_next(RBTREE_NULL) == nullptr);
    CHECK(rbtree_previous(RBTREE_NULL) == nullptr);
    traverse_postorder(static_cast<rbtree_t*>(0), collect_postorder,
                       &callback_count);
    traverse_postorder(&tree, static_cast<void (*)(rbnode_t*, void*)>(0),
                       &callback_count);
    CHECK(callback_count == 0U);

    tree.cmp = 0;
    CHECK(rbtree_insert(&tree, &item.node) == nullptr);
    CHECK(rbtree_search(&tree, &key) == nullptr);
    CHECK(rbtree_delete(&tree, &key) == nullptr);
    rbtree_init(&tree, compare_int);
    tree.count = std::numeric_limits<std::size_t>::max();
    CHECK(rbtree_insert(&tree, &item.node) == nullptr);
    tree.count = 0U;
    CHECK(validate_tree(tree, 0U));
}

TEST_CASE("rbtree inserts unique keys and maintains sorted traversal",
          "[rbtree][insert][iteration]")
{
    rbtree_t tree = {};
    std::array<TestItem, 9U> items = {};
    TestItem duplicate = {};
    const std::array<int, 9U> keys = {{5, 2, 8, 1, 3, 7, 9, 6, 4}};
    const std::vector<int> expected = {1, 2, 3, 4, 5, 6, 7, 8, 9};

    rbtree_init(&tree, compare_int);
    for (std::size_t index = 0U; index < items.size(); ++index) {
        prepare_item(items[index], keys[index]);
        REQUIRE(rbtree_insert(&tree, &items[index].node) == &items[index].node);
        CHECK(validate_tree(tree, index + 1U));
    }

    prepare_item(duplicate, 5);
    CHECK(rbtree_insert(&tree, &duplicate.node) == nullptr);
    CHECK(tree.count == items.size());
    CHECK(duplicate.node.parent == nullptr);
    CHECK(duplicate.node.left == nullptr);
    CHECK(duplicate.node.right == nullptr);
    CHECK(validate_tree(tree, items.size()));

    CHECK(inorder_keys(&tree) == expected);
    CHECK(item_key(rbtree_first(&tree)) == 1);
    CHECK(item_key(rbtree_last(&tree)) == 9);
    CHECK(rbtree_previous(rbtree_first(&tree)) == RBTREE_NULL);
    CHECK(rbtree_next(rbtree_last(&tree)) == RBTREE_NULL);

    std::vector<int> reverse;
    for (rbnode_t* node = rbtree_last(&tree); node != RBTREE_NULL;
         node = rbtree_previous(node)) {
        reverse.push_back(item_key(node));
    }
    CHECK(reverse == std::vector<int>({9, 8, 7, 6, 5, 4, 3, 2, 1}));

    std::vector<int> macro_order;
    TestItem* item = static_cast<TestItem*>(0);
    RBTREE_FOR(item, TestItem*, &tree) {
        macro_order.push_back(item->key);
    }
    CHECK(macro_order == expected);

    for (int value : expected) {
        int lookup = value;
        rbnode_t* found = rbtree_search(&tree, &lookup);
        REQUIRE(found != nullptr);
        CHECK(item_key(found) == value);
    }
    for (const int missing : {-10, 0, 10}) {
        int lookup = missing;
        CHECK(rbtree_search(&tree, &lookup) == nullptr);
    }
}

TEST_CASE("rbtree find_less_equal returns exact keys or their predecessor",
          "[rbtree][search]")
{
    rbtree_t tree = {};
    std::array<TestItem, 4U> items = {};
    const std::array<int, 4U> keys = {{10, 20, 30, 40}};
    const std::array<int, 7U> queries = {{5, 10, 15, 20, 35, 40, 99}};
    const std::array<int, 7U> expected = {{-1, 10, 10, 20, 30, 40, 40}};
    const std::array<int, 7U> exact = {{0, 1, 0, 1, 0, 1, 0}};

    rbtree_init(&tree, compare_int);
    for (std::size_t index = 0U; index < items.size(); ++index) {
        prepare_item(items[index], keys[index]);
        REQUIRE(rbtree_insert(&tree, &items[index].node) != nullptr);
    }

    for (std::size_t index = 0U; index < queries.size(); ++index) {
        rbnode_t* result = &items[0].node;
        int query = queries[index];

        CHECK(rbtree_find_less_equal(&tree, &query, &result) == exact[index]);
        if (expected[index] < 0) {
            CHECK(result == nullptr);
        } else {
            REQUIRE(result != nullptr);
            CHECK(item_key(result) == expected[index]);
        }
    }
    CHECK(validate_tree(tree, items.size()));
}

TEST_CASE("rbtree deletion handles leaves, one-child nodes, roots, and reuse",
          "[rbtree][delete]")
{
    rbtree_t tree = {};
    std::array<TestItem, 17U> items = {};
    const std::array<int, 17U> keys =
        {{41, 20, 65, 11, 29, 50, 75, 4, 14, 25, 33, 47, 54, 70, 82, 1, 6}};
    const std::array<int, 17U> deletion_order =
        {{1, 14, 20, 65, 41, 4, 50, 82, 11, 29, 75, 6, 25, 33, 47, 54, 70}};

    rbtree_init(&tree, compare_int);
    for (std::size_t index = 0U; index < items.size(); ++index) {
        prepare_item(items[index], keys[index]);
        REQUIRE(rbtree_insert(&tree, &items[index].node) != nullptr);
    }
    REQUIRE(validate_tree(tree, items.size()));

    /* This lies between 50 and 54; a miss must not delete its predecessor. */
    int missing = 52;
    CHECK(rbtree_search(&tree, &missing) == nullptr);
    CHECK(rbtree_delete(&tree, &missing) == nullptr);
    CHECK(tree.count == items.size());

    std::size_t remaining = items.size();
    for (int key : deletion_order) {
        TestItem* expected_item = static_cast<TestItem*>(0);
        rbnode_t* deleted;

        for (TestItem& item : items) {
            if (item.key == key) {
                expected_item = &item;
                break;
            }
        }
        REQUIRE(expected_item != nullptr);
        deleted = rbtree_delete(&tree, &expected_item->key);
        REQUIRE(deleted == &expected_item->node);
        CHECK(deleted->parent == RBTREE_NULL);
        CHECK(deleted->left == RBTREE_NULL);
        CHECK(deleted->right == RBTREE_NULL);
        --remaining;
        CHECK(tree.count == remaining);
        CHECK(validate_tree(tree, remaining));
        CHECK(rbtree_search(&tree, &expected_item->key) == nullptr);
    }

    CHECK(tree.root == RBTREE_NULL);
    CHECK(rbtree_first(&tree) == RBTREE_NULL);
    CHECK(rbtree_last(&tree) == RBTREE_NULL);
    CHECK(rbtree_delete(&tree, &missing) == nullptr);

    prepare_item(items[0], 41);
    REQUIRE(rbtree_insert(&tree, &items[0].node) == &items[0].node);
    CHECK(tree.count == 1U);
    CHECK(validate_tree(tree, 1U));
    CHECK(rbtree_delete(&tree, &items[0].key) == &items[0].node);
    CHECK(validate_tree(tree, 0U));
}

TEST_CASE("rbtree postorder traversal visits children before parents",
          "[rbtree][traversal]")
{
    rbtree_t tree = {};
    std::array<TestItem, 7U> items = {};
    const std::array<int, 7U> keys = {{4, 2, 6, 1, 3, 5, 7}};
    std::vector<rbnode_t*> visited;

    rbtree_init(&tree, compare_int);
    for (std::size_t index = 0U; index < items.size(); ++index) {
        prepare_item(items[index], keys[index]);
        REQUIRE(rbtree_insert(&tree, &items[index].node) != nullptr);
    }

    traverse_postorder(&tree, collect_postorder, &visited);
    REQUIRE(visited.size() == items.size());
    for (std::size_t index = 0U; index < visited.size(); ++index) {
        const rbnode_t* node = visited[index];
        const std::size_t left_index = find_node_index(visited, node->left);
        const std::size_t right_index = find_node_index(visited, node->right);
        const std::size_t parent_index = find_node_index(visited, node->parent);

        CHECK((left_index == visited.size() || left_index < index));
        CHECK((right_index == visited.size() || right_index < index));
        CHECK((parent_index == visited.size() || parent_index > index));
    }
    CHECK(validate_tree(tree, items.size()));
}

TEST_CASE("rbtree matches an ordered set during mixed operations",
          "[rbtree][property]")
{
    constexpr int key_min = -128;
    constexpr std::size_t key_count = 257U;
    constexpr std::size_t operation_count = 3000U;
    rbtree_t tree = {};
    std::array<TestItem, key_count> items = {};
    std::set<int> expected;
    std::uint32_t state = 0xC001D00DU;

    rbtree_init(&tree, compare_int);
    for (std::size_t index = 0U; index < items.size(); ++index) {
        prepare_item(items[index], key_min + static_cast<int>(index));
    }

    for (std::size_t operation = 0U; operation < operation_count; ++operation) {
        int key;
        state = state * 1664525U + 1013904223U;
        key = key_min + static_cast<int>(state % key_count);
        const std::size_t item_index = static_cast<std::size_t>(key - key_min);
        rbnode_t* node;

        if ((state & 3U) != 0U) {
            const bool inserted = expected.insert(key).second;
            node = rbtree_insert(&tree, &items[item_index].node);
            CHECK((node != nullptr) == inserted);
            if (inserted) {
                CHECK(node == &items[item_index].node);
            }
        } else {
            const bool present = expected.erase(key) != 0U;
            node = rbtree_delete(&tree, &key);
            CHECK((node != nullptr) == present);
            if (present) {
                CHECK(node == &items[item_index].node);
                CHECK(node->parent == RBTREE_NULL);
                CHECK(node->left == RBTREE_NULL);
                CHECK(node->right == RBTREE_NULL);
            }
        }

        CHECK(validate_tree(tree, expected.size()));
        CHECK(inorder_keys(&tree) == std::vector<int>(expected.begin(), expected.end()));

        state = state * 1664525U + 1013904223U;
        key = key_min + static_cast<int>(state % key_count);
        node = rbtree_search(&tree, &key);
        if (expected.find(key) == expected.end()) {
            CHECK(node == nullptr);
        } else {
            REQUIRE(node != nullptr);
            CHECK(item_key(node) == key);
        }
    }

    CHECK(validate_tree(tree, expected.size()));
}
