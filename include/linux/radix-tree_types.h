/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _LINUX_RADIX_TREE_TYPES_H
#define _LINUX_RADIX_TREE_TYPES_H

#include <linux/xarray_types.h>

/* Keep unconverted code working */
#define radix_tree_root		xarray
#define radix_tree_node		xa_node

#define RADIX_TREE_INIT(name, mask)	XARRAY_INIT(name, mask)

#define RADIX_TREE(name, mask) \
	struct radix_tree_root name = RADIX_TREE_INIT(name, mask)

#define INIT_RADIX_TREE(root, mask) xa_init_flags(root, mask)

static inline bool radix_tree_empty(const struct radix_tree_root *root)
{
	return root->xa_head == NULL;
}

/**
 * struct radix_tree_iter - radix tree iterator state
 *
 * @index:	index of current slot
 * @next_index:	one beyond the last index for this chunk
 * @tags:	bit-mask for tag-iterating
 * @node:	node that contains current slot
 *
 * This radix tree iterator works in terms of "chunks" of slots.  A chunk is a
 * subinterval of slots contained within one radix tree leaf node.  It is
 * described by a pointer to its first slot and a struct radix_tree_iter
 * which holds the chunk's position in the tree and its size.  For tagged
 * iteration radix_tree_iter also holds the slots' bit-mask for one chosen
 * radix tree tag.
 */
struct radix_tree_iter {
	unsigned long	index;
	unsigned long	next_index;
	unsigned long	tags;
	struct radix_tree_node *node;
};

/* The IDR tag is stored in the low bits of xa_flags */
#define ROOT_IS_IDR	((__force gfp_t)4)
/* The top bits of xa_flags are used to store the root tags */
#define ROOT_TAG_SHIFT	(__GFP_BITS_SHIFT)

#endif /* _LINUX_RADIX_TREE_TYPES_H */
