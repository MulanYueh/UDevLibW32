/*
 * rbtree.h -- generic red-black tree
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
 * Red black tree. Implementation taken from NSD 3.0.5, adjusted for use
 * in unbound (memory allocation, logging and so on).
 *
 * The tree is an intrusive index: it never allocates, copies, or frees user
 * records.  Embed rbnode_t as the first member of a record and put the sort
 * key in rbnode_t::key:
 *
 *     +----------------------+------------------------+
 *     | rbnode_t (tree links)| caller-owned record     |
 *     +----------------------+------------------------+
 *       ^ cast returned node back to the containing record
 *
 * rbtree_insert initializes the embedded links.  rbtree_delete only unlinks
 * the node and returns it; the caller remains responsible for its lifetime.
 */

#ifndef UTIL_RBTREE_H_
#define	UTIL_RBTREE_H_

/* The tree only needs size_t, but include its definition for standalone
 * builds as well as when this header is included before a platform header. */
#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <stddef.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * This structure must be the first member of the data structure in
 * the rbtree.  This allows easy casting between an rbnode_t and the
 * user data (poor man's inheritance).
 */
typedef struct rbnode_t rbnode_t;
/**
 * The rbnode_t struct definition.
 */
struct rbnode_t {
	/** parent in rbtree, RBTREE_NULL for root */
	rbnode_t   *parent;
	/** left node (smaller items) */
	rbnode_t   *left;
	/** right node (larger items) */
	rbnode_t   *right;
	/** pointer to sorting key */
	const void *key;
	/** colour of this node */
	unsigned char color;
};

/**
 * The shared leaf sentinel.  Empty child links point here instead of using
 * C NULL, which lets balancing code read sentinel->color and its child
 * colors without special cases.  Never cast or dereference it as user data;
 * rbtree_first/last return this value for an empty tree.
 */
#define	RBTREE_NULL &rbtree_null_node
/** the global empty node */
extern	rbnode_t	rbtree_null_node;

/** An entire red black tree */
typedef struct rbtree_t rbtree_t;
/** definition for tree struct */
struct rbtree_t {
	/** The root of the red-black tree */
	rbnode_t    *root;

	/** The number of the nodes in the tree */
	size_t       count;

	/**
	 * Key compare function. <0,0,>0 like strcmp.  The function must define a
	 * strict ordering for every key inserted into this tree; returning zero
	 * means that the key is already present and insertion is rejected.
	 */
	int (*cmp) (const void *, const void *);
};

/** 
 * Init a new tree (malloced by caller) with given key compare function. 
 * @param rbtree: uninitialised memory for new tree, returned empty.
 * @param cmpf: compare function (like strcmp) takes pointers to two keys.
 *
 * No allocation is performed.  The caller owns the rbtree_t object and must
 * initialize it before any other operation.
 */
void rbtree_init(rbtree_t *rbtree, int (*cmpf)(const void *, const void *));

/** 
 * Insert data into the tree. 
 * @param rbtree: tree to insert to.
 * @param data: element to insert. 
 * @return: data ptr or NULL if key already present.  On success the node's
 *   parent/left/right/color fields are overwritten by the tree.
 */
rbnode_t *rbtree_insert(rbtree_t *rbtree, rbnode_t *data);

/**
 * Delete element from tree.
 * @param rbtree: tree to delete from.
 * @param key: key of item to delete.
 * @return: node that is now unlinked from the tree. User to delete it. 
 * returns 0 if node not present 
 *
 * Deletion may move other nodes while rebalancing, but the returned node is
 * fully detached and can be released by the caller after the function.
 */
rbnode_t *rbtree_delete(rbtree_t *rbtree, const void *key);

/**
 * Find key in tree. Returns NULL if not found.
 * @param rbtree: tree to find in.
 * @param key: key that must match.
 * @return: node that fits or NULL.
 */
rbnode_t *rbtree_search(rbtree_t *rbtree, const void *key);

/**
 * Find, but match does not have to be exact.
 * @param rbtree: tree to find in.
 * @param key: key to find position of.
 * @param result: set to the exact node if present, otherwise to element that
 *   precedes the position of key in the tree. NULL if no smaller element.
 * @return: true if exact match in result. Else result points to <= element,
 * or NULL if key is smaller than the smallest key. 
 *
 * This predecessor operation is useful for interval indexes: find the
 * greatest allocation base not greater than an address, then validate the
 * offset with subtraction rather than base + length.
 */
int rbtree_find_less_equal(rbtree_t *rbtree, const void *key, 
	rbnode_t **result);

/**
 * Returns first (smallest) node in the tree
 * @param rbtree: tree
 * @return: smallest element, or RBTREE_NULL if tree is empty.  The sentinel
 *   is not a caller-owned node and must not be dereferenced as user data.
 */
rbnode_t *rbtree_first(rbtree_t *rbtree);

/**
 * Returns last (largest) node in the tree
 * @param rbtree: tree
 * @return: largest element, or RBTREE_NULL if tree is empty.  The sentinel
 *   is not a caller-owned node and must not be dereferenced as user data.
 */
rbnode_t *rbtree_last(rbtree_t *rbtree);

/**
 * Returns next larger node in the tree
 * @param rbtree: tree
 * @return: next larger element or NULL if no larger in tree.
 */
rbnode_t *rbtree_next(rbnode_t *rbtree);

/**
 * Returns previous smaller node in the tree
 * @param rbtree: tree
 * @return: previous smaller element or NULL if no previous in tree.
 */
rbnode_t *rbtree_previous(rbnode_t *rbtree);

/**
 * Call with node=variable of struct* with rbnode_t as first element.
 * with type is the type of a pointer to that struct. 
 */
#define RBTREE_FOR(node, type, rbtree) \
	for(node=(type)rbtree_first(rbtree); \
		(rbnode_t*)node != RBTREE_NULL; \
		node = (type)rbtree_next((rbnode_t*)node))

/**
 * Call function for all elements in the redblack tree, such that
 * leaf elements are called before parent elements. So that all
 * elements can be safely free()d.
 * Note that your function must not remove the nodes from the tree.
 * Since that may trigger rebalances of the rbtree.
 * @param tree: the tree
 * @param func: function called with element and user arg.
 * 	The function must not alter the rbtree.
 * @param arg: user argument.
 */
void traverse_postorder(rbtree_t* tree, void (*func)(rbnode_t*, void*),
	void* arg);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UTIL_RBTREE_H_ */
