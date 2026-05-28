/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_KHO_RADIX_TREE_H
#define _LINUX_KHO_RADIX_TREE_H

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/mutex_types.h>
#include <linux/types.h>

/**
 * DOC: Kexec Handover Radix Tree
 *
 * This is a radix tree implementation for tracking physical memory pages
 * across kexec transitions. It was developed for the KHO mechanism but is
 * designed for broader use by any subsystem that needs to preserve pages.
 *
 * The radix tree is a multi-level tree where leaf nodes are bitmaps
 * representing individual pages. To allow pages of different sizes (orders)
 * to be stored efficiently in a single tree, it uses a unique key encoding
 * scheme. Each key is an unsigned long that combines a page's physical
 * address and its order.
 *
 * Client code must initialize the tree using kho_radix_tree_init(). Pass
 * a physical address to restore a tree preserved across kexec, or 0 to
 * allocate a fresh empty tree. The tree uses data structures defined in
 * the KHO ABI, `include/linux/kho/abi/kexec_handover.h`.
 */

struct kho_radix_node;

struct kho_radix_tree {
	struct kho_radix_node *root;
	struct mutex lock; /* protects the tree's structure and root pointer */
	bool frozen;
};

/**
 * struct kho_radix_walk_cb - Callbacks for KHO radix tree walk.
 * @key:      Called on each present key in the radix tree.
 * @table:    Called on each table of the radix tree itself. Receives the
 *            physical address of the page containing the table.
 *
 * For each callback, a return value of 0 continues the walk and a non-zero
 * return value is directly returned to the caller.
 */
struct kho_radix_walk_cb {
	int (*key)(unsigned long key, void *data);
	int (*table)(phys_addr_t phys, void *data);
};

#ifdef CONFIG_KEXEC_HANDOVER

int kho_radix_add_key(struct kho_radix_tree *tree, unsigned long key);
int kho_radix_del_key(struct kho_radix_tree *tree, unsigned long key);
int kho_radix_walk_tree(struct kho_radix_tree *tree,
			const struct kho_radix_walk_cb *cb, void *data);
int kho_radix_init_tree(struct kho_radix_tree *tree, struct kho_radix_node *root);
void kho_radix_destroy_tree(struct kho_radix_tree *tree);
int kho_radix_tree_freeze(struct kho_radix_tree *tree);

#else  /* #ifdef CONFIG_KEXEC_HANDOVER */

static inline int kho_radix_add_key(struct kho_radix_tree *tree, unsigned long key)
{
	return -EOPNOTSUPP;
}

static inline int kho_radix_del_key(struct kho_radix_tree *tree,
				     unsigned long key)
{
	return -EOPNOTSUPP;
}

static inline int kho_radix_walk_tree(struct kho_radix_tree *tree,
				      const struct kho_radix_walk_cb *cb, void *data)
{
	return -EOPNOTSUPP;
}

static inline int kho_radix_init_tree(struct kho_radix_tree *tree,
				      struct kho_radix_node *root)
{
	return 0;
}

static inline void kho_radix_destroy_tree(struct kho_radix_tree *tree) { }

static inline int kho_radix_tree_freeze(struct kho_radix_tree *tree)
{
	return -EOPNOTSUPP;
}

#endif /* #ifdef CONFIG_KEXEC_HANDOVER */

#endif	/* _LINUX_KHO_RADIX_TREE_H */
