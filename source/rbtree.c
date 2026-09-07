/*
 * rbtree.c -- generic red black tree
 *
 * Copyright (c) 2001-2007, NLnet Labs. All rights reserved.
 * 
 * This software is open source under the BSD license.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * \file
 * Implementation of a redblack tree.
 */

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <stddef.h>
#endif
#include "rbtree.h"

#define log_assert(x)

/** Node colour black */
#define	BLACK	0
/** Node colour red */
#define	RED	1

#ifndef NULL
#define NULL ((void*)0)
#endif

/*
 * One shared black leaf is used by every tree.  A black sentinel means the
 * balancing routines can inspect sibling/child colors without checking for
 * NULL at every branch.  Its links point to itself and are never changed.
 */
rbnode_t	rbtree_null_node = {
	RBTREE_NULL,		/* Parent.  */
	RBTREE_NULL,		/* Left.  */
	RBTREE_NULL,		/* Right.  */
	NULL,			/* Key.  */
	BLACK			/* Color.  */
};

/** rotate subtree left (to preserve redblack property) */
static void rbtree_rotate_left(rbtree_t *rbtree, rbnode_t *node);
/** rotate subtree right (to preserve redblack property) */
static void rbtree_rotate_right(rbtree_t *rbtree, rbnode_t *node);
/** Fixup node colours when insert happened */
static void rbtree_insert_fixup(rbtree_t *rbtree, rbnode_t *node);
/** Fixup node colours when delete happened */
static void rbtree_delete_fixup(rbtree_t* rbtree, rbnode_t* child, rbnode_t* child_parent);

/*
 * Initializes a new red black tree.
 *
 */
void 
rbtree_init(rbtree_t *rbtree, int (*cmpf)(const void *, const void *))
{
	if (rbtree == NULL) {
		return;
	}
	/* The caller owns the storage; initialization only points the root at the
	 * shared black leaf and records the ordering function. */
	rbtree->root = RBTREE_NULL;
	rbtree->count = 0;
	rbtree->cmp = cmpf;
}

/*
 * Rotates the node to the left.
 *
 */
static void
rbtree_rotate_left(rbtree_t *rbtree, rbnode_t *node)
{
	/*
	 * Before:                 After:
	 *       node                 right
	 *          \                /    \
	 *          right           node   ...
	 *         /
	 *       middle
	 *
	 * Move the middle subtree first, then reconnect the promoted node to the
	 * former parent (or make it the root).  Rotation changes shape only; color
	 * fix-up is handled by the caller.
	 */
	rbnode_t *right = node->right;
	node->right = right->left;
	if (right->left != RBTREE_NULL)
		right->left->parent = node;

	right->parent = node->parent;

	if (node->parent != RBTREE_NULL) {
		if (node == node->parent->left) {
			node->parent->left = right;
		} else  {
			node->parent->right = right;
		}
	} else {
		rbtree->root = right;
	}
	right->left = node;
	node->parent = right;
}

/*
 * Rotates the node to the right.
 *
 */
static void
rbtree_rotate_right(rbtree_t *rbtree, rbnode_t *node)
{
	/* Mirror image of rbtree_rotate_left. */
	rbnode_t *left = node->left;
	node->left = left->right;
	if (left->right != RBTREE_NULL)
		left->right->parent = node;

	left->parent = node->parent;

	if (node->parent != RBTREE_NULL) {
		if (node == node->parent->right) {
			node->parent->right = left;
		} else  {
			node->parent->left = left;
		}
	} else {
		rbtree->root = left;
	}
	left->right = node;
	node->parent = left;
}

static void
rbtree_insert_fixup(rbtree_t *rbtree, rbnode_t *node)
{
	rbnode_t	*uncle = RBTREE_NULL;

	/*
	 * A new node is red, so only a red parent can violate the invariant that a
	 * red node must have black children.  The three standard cases are handled
	 * by recoloring when the uncle is red, or rotating/recoloring when it is
	 * black.  The loop moves toward the root and therefore terminates.
	 */
	/* While not at the root and need fixing... */
	while (node != rbtree->root && node->parent->color == RED) {
		/* If our parent is left child of our grandparent... */
		if (node->parent == node->parent->parent->left) {
			uncle = node->parent->parent->right;

			/* If our uncle is red... */
			if (uncle->color == RED) {
				/* Paint the parent and the uncle black... */
				node->parent->color = BLACK;
				uncle->color = BLACK;

				/* And the grandparent red... */
				node->parent->parent->color = RED;

				/* And continue fixing the grandparent */
				node = node->parent->parent;
			} else {				/* Our uncle is black... */
				/* Are we the right child? */
				if (node == node->parent->right) {
					node = node->parent;
					rbtree_rotate_left(rbtree, node);
				}
				/* Now we're the left child, repaint and rotate... */
				node->parent->color = BLACK;
				node->parent->parent->color = RED;
				rbtree_rotate_right(rbtree, node->parent->parent);
			}
		} else {
			uncle = node->parent->parent->left;

			/* If our uncle is red... */
			if (uncle->color == RED) {
				/* Paint the parent and the uncle black... */
				node->parent->color = BLACK;
				uncle->color = BLACK;

				/* And the grandparent red... */
				node->parent->parent->color = RED;

				/* And continue fixing the grandparent */
				node = node->parent->parent;
			} else {				/* Our uncle is black... */
				/* Are we the right child? */
				if (node == node->parent->left) {
					node = node->parent;
					rbtree_rotate_right(rbtree, node);
				}
				/* Now we're the right child, repaint and rotate... */
				node->parent->color = BLACK;
				node->parent->parent->color = RED;
				rbtree_rotate_left(rbtree, node->parent->parent);
			}
		}
	}
	rbtree->root->color = BLACK;
}


/*
 * Inserts a node into a red black tree.
 *
 * Returns NULL on failure or the pointer to the newly added node
 * otherwise.
 */
rbnode_t *
rbtree_insert (rbtree_t *rbtree, rbnode_t *data)
{
	/* XXX Not necessary, but keeps compiler quiet... */
	int r = 0;
	rbnode_t *node = RBTREE_NULL;
	rbnode_t *parent = RBTREE_NULL;

	if (rbtree == NULL || data == NULL || data == RBTREE_NULL ||
	    rbtree->cmp == NULL || rbtree->count == (size_t)-1) {
		return NULL;
	}

	/*
	 * First perform a normal binary-search-tree insertion.  The comparator is
	 * also the uniqueness policy: equal keys are rejected before any links are
	 * changed.  The new red node is then repaired by rbtree_insert_fixup.
	 */
	/* We start at the root of the tree */
	node = rbtree->root;

	/* Lets find the new parent... */
	while (node != RBTREE_NULL) {
		/* Compare two keys, do we have a duplicate? */
		if ((r = rbtree->cmp(data->key, node->key)) == 0) {
			return NULL;
		}
		parent = node;

		if (r < 0) {
			node = node->left;
		} else {
			node = node->right;
		}
	}

	/* Initialize the new node */
	data->parent = parent;
	data->left = data->right = RBTREE_NULL;
	data->color = RED;
	rbtree->count++;

	/* Insert it into the tree... */
	if (parent != RBTREE_NULL) {
		if (r < 0) {
			parent->left = data;
		} else {
			parent->right = data;
		}
	} else {
		rbtree->root = data;
	}

	/* Fix up the red-black properties... */
	rbtree_insert_fixup(rbtree, data);

	return data;
}

/*
 * Searches the red black tree, returns the data if key is found or NULL otherwise.
 *
 */
rbnode_t *
rbtree_search (rbtree_t *rbtree, const void *key)
{
	rbnode_t *node = NULL;

	if (rbtree == NULL || rbtree->cmp == NULL) {
		return NULL;
	}

	/* find_less_equal also returns a predecessor for a miss.  Search has
	 * stricter exact-match semantics, so honor its boolean result; otherwise a
	 * lookup for a missing key could accidentally return/delete its predecessor. */
	if (!rbtree_find_less_equal(rbtree, key, &node)) {
		return NULL;
	}
	return node;
}

/** Replace one subtree root without ever modifying the shared sentinel. */
static void
rbtree_transplant(rbtree_t *rbtree, rbnode_t *old_node, rbnode_t *new_node)
{
	if (old_node->parent == RBTREE_NULL) {
		rbtree->root = new_node;
	} else if (old_node == old_node->parent->left) {
		old_node->parent->left = new_node;
	} else {
		old_node->parent->right = new_node;
	}
	if (new_node != RBTREE_NULL) {
		new_node->parent = old_node->parent;
	}
}

static rbnode_t *
rbtree_minimum(rbnode_t *node)
{
	while (node->left != RBTREE_NULL) {
		node = node->left;
	}
	return node;
}

rbnode_t *
rbtree_delete(rbtree_t *rbtree, const void *key)
{
	rbnode_t *to_delete = NULL;
	rbnode_t *replacement = RBTREE_NULL;
	rbnode_t *child = RBTREE_NULL;
	rbnode_t *child_parent = RBTREE_NULL;
	unsigned char removed_color = BLACK;

	if (rbtree == NULL || rbtree->cmp == NULL || rbtree->count == 0) {
		return NULL;
	}
	to_delete = rbtree_search(rbtree, key);
	if (to_delete == NULL) {
		return NULL;
	}

	/* Move the in-order successor into place when both children exist.  The
	 * requested node itself is still the one detached and returned. */
	replacement = to_delete;
	removed_color = replacement->color;
	if (to_delete->left == RBTREE_NULL) {
		child = to_delete->right;
		child_parent = to_delete->parent;
		rbtree_transplant(rbtree, to_delete, to_delete->right);
	} else if (to_delete->right == RBTREE_NULL) {
		child = to_delete->left;
		child_parent = to_delete->parent;
		rbtree_transplant(rbtree, to_delete, to_delete->left);
	} else {
		replacement = rbtree_minimum(to_delete->right);
		removed_color = replacement->color;
		child = replacement->right;
		if (replacement->parent == to_delete) {
			child_parent = replacement;
			if (child != RBTREE_NULL) {
				child->parent = replacement;
			}
		} else {
			child_parent = replacement->parent;
			rbtree_transplant(rbtree, replacement, replacement->right);
			replacement->right = to_delete->right;
			replacement->right->parent = replacement;
		}
		rbtree_transplant(rbtree, to_delete, replacement);
		replacement->left = to_delete->left;
		replacement->left->parent = replacement;
		replacement->color = to_delete->color;
	}

	if (removed_color == BLACK) {
		rbtree_delete_fixup(rbtree, child, child_parent);
	}
	--rbtree->count;

	to_delete->parent = RBTREE_NULL;
	to_delete->left = RBTREE_NULL;
	to_delete->right = RBTREE_NULL;
	to_delete->color = BLACK;
	return to_delete;
}

static void
rbtree_delete_fixup(rbtree_t *rbtree, rbnode_t *child,
	                 rbnode_t *child_parent)
{
	rbnode_t *sibling = RBTREE_NULL;

	/* child_parent is carried separately because all leaf links share one
	 * immutable sentinel whose parent field cannot describe every leaf. */
	while (child != rbtree->root && child->color == BLACK) {
		if (child_parent == RBTREE_NULL) {
			break;
		}
		if (child == child_parent->left) {
			sibling = child_parent->right;
			if (sibling->color == RED) {
				sibling->color = BLACK;
				child_parent->color = RED;
				rbtree_rotate_left(rbtree, child_parent);
				sibling = child_parent->right;
			}
			if (sibling->left->color == BLACK &&
			    sibling->right->color == BLACK) {
				if (sibling != RBTREE_NULL) {
					sibling->color = RED;
				}
				child = child_parent;
				child_parent = child->parent;
			} else {
				if (sibling->right->color == BLACK) {
					sibling->left->color = BLACK;
					sibling->color = RED;
					rbtree_rotate_right(rbtree, sibling);
					sibling = child_parent->right;
				}
				sibling->color = child_parent->color;
				child_parent->color = BLACK;
				sibling->right->color = BLACK;
				rbtree_rotate_left(rbtree, child_parent);
				child = rbtree->root;
				child_parent = RBTREE_NULL;
			}
		} else {
			sibling = child_parent->left;
			if (sibling->color == RED) {
				sibling->color = BLACK;
				child_parent->color = RED;
				rbtree_rotate_right(rbtree, child_parent);
				sibling = child_parent->left;
			}
			if (sibling->right->color == BLACK &&
			    sibling->left->color == BLACK) {
				if (sibling != RBTREE_NULL) {
					sibling->color = RED;
				}
				child = child_parent;
				child_parent = child->parent;
			} else {
				if (sibling->left->color == BLACK) {
					sibling->right->color = BLACK;
					sibling->color = RED;
					rbtree_rotate_left(rbtree, sibling);
					sibling = child_parent->left;
				}
				sibling->color = child_parent->color;
				child_parent->color = BLACK;
				sibling->left->color = BLACK;
				rbtree_rotate_right(rbtree, child_parent);
				child = rbtree->root;
				child_parent = RBTREE_NULL;
			}
		}
	}
	if (child != RBTREE_NULL) {
		child->color = BLACK;
	}
}

int
rbtree_find_less_equal(rbtree_t *rbtree, const void *key, rbnode_t **result)
{
	int r = 0;
	rbnode_t *node = RBTREE_NULL;

	if (result == NULL) {
		return 0;
	}
	*result = NULL;
	if (rbtree == NULL || rbtree->cmp == NULL) {
		return 0;
	}
	if (rbtree->root == NULL) {
		return 0;
	}
	
	/*
	 * Walk as in a binary search.  On an exact match return immediately; when
	 * moving right, remember the current node because it is the best predecessor
	 * seen so far.  Moving left does not discard that predecessor.  The result
	 * is therefore the greatest key <= the requested key, if one exists.
	 */
	/* We start at root... */
	node = rbtree->root;
	if (node == NULL) {
		return 0;
	}

	/* While there are children... */
	while (node != RBTREE_NULL) {
		r = rbtree->cmp(key, node->key);
		if (r == 0) {
			/* Exact match */
			*result = node;
			return 1;
		} 
		if (r < 0) {
			node = node->left;
		} else {
			/* Temporary match */
			*result = node;
			node = node->right;
		}
	}
	return 0;
}

/*
 * Finds the first element in the red black tree
 *
 */
rbnode_t *
rbtree_first (rbtree_t *rbtree)
{
	rbnode_t *node = RBTREE_NULL;

	if (rbtree == NULL) {
		return RBTREE_NULL;
	}

	/* Following left links reaches the minimum key.  For an empty tree root is
	 * already the sentinel, which is returned unchanged. */
	if (rbtree->root == NULL) {
		return RBTREE_NULL;
	}
	for (node = rbtree->root; node->left != RBTREE_NULL; node = node->left);
	return node;
}

rbnode_t *
rbtree_last (rbtree_t *rbtree)
{
	rbnode_t *node = RBTREE_NULL;

	if (rbtree == NULL) {
		return RBTREE_NULL;
	}

	/* Symmetric walk for the maximum key; see rbtree_first for empty-tree
	 * behavior. */
	if (rbtree->root == NULL) {
		return RBTREE_NULL;
	}
	for (node = rbtree->root; node->right != RBTREE_NULL; node = node->right);
	return node;
}

/*
 * Returns the next node...
 *
 */
rbnode_t *
rbtree_next (rbnode_t *node)
{
	rbnode_t *parent = RBTREE_NULL;

	if (node == NULL || node == RBTREE_NULL) {
		return NULL;
	}

	/* The successor is the leftmost node in the right subtree.  If there is no
	 * right subtree, climb parents until coming from a left edge. */
	if (node->right != RBTREE_NULL) {
		/* One right, then keep on going left... */
		for (node = node->right; node->left != RBTREE_NULL; node = node->left);
	} else {
		parent = node->parent;
		while (parent != RBTREE_NULL && node == parent->right) {
			node = parent;
			parent = parent->parent;
		}
		node = parent;
	}
	return node;
}

rbnode_t *
rbtree_previous(rbnode_t *node)
{
	rbnode_t *parent = RBTREE_NULL;

	if (node == NULL || node == RBTREE_NULL) {
		return NULL;
	}

	/* Mirror image of rbtree_next: use the rightmost node in the left subtree,
	 * or climb until coming from a right edge. */
	if (node->left != RBTREE_NULL) {
		/* One left, then keep on going right... */
		for (node = node->left; node->right != RBTREE_NULL; node = node->right);
	} else {
		parent = node->parent;
		while (parent != RBTREE_NULL && node == parent->left) {
			node = parent;
			parent = parent->parent;
		}
		node = parent;
	}
	return node;
}

/** recursive descent traverse */
static void 
traverse_post(void (*func)(rbnode_t*, void*), void* arg, rbnode_t* node)
{
	/* Post-order visits children before their parent, so a callback can release
	 * leaf records safely after it has finished using their descendants. */
	if(!node || node == RBTREE_NULL)
		return;
	/* recurse */
	traverse_post(func, arg, node->left);
	traverse_post(func, arg, node->right);
	/* call user func */
	(*func)(node, arg);
}

void 
traverse_postorder(rbtree_t* tree, void (*func)(rbnode_t*, void*), void* arg)
{
	if (tree == NULL || func == NULL) {
		return;
	}
	traverse_post(func, arg, tree->root);
}
