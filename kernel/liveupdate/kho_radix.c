// SPDX-License-Identifier: GPL-2.0-only
/*
 * kho_radix.c - KHO radix tree tracker for preserved memory pages
 * Copyright (C) 2025 Microsoft Corporation, Mike Rapoport <rppt@kernel.org>
 * Copyright (C) 2025 Pasha Tatashin <pasha.tatashin@soleen.com>
 * Copyright (C) 2026 Google LLC, Jason Miu <jasonmiu@google.com>
 */

#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kho/abi/kexec_handover.h>
#include <linux/kho/radix_tree.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/types.h>

/**
 * kho_radix_encode_key - Encodes a physical address and order into a radix key.
 * @phys: The physical address of the page.
 * @order: The order of the page.
 *
 * This function combines a page's physical address and its order into a
 * single unsigned long, which is used as a key for all radix tree
 * operations.
 *
 * Return: The encoded unsigned long radix key.
 */
static unsigned long kho_radix_encode_key(phys_addr_t phys, unsigned int order)
{
	/* Order bits part */
	unsigned long h = 1UL << (KHO_ORDER_0_LOG2 - order);
	/* Shifted physical address part */
	unsigned long l = phys >> (PAGE_SHIFT + order);

	return h | l;
}

/**
 * kho_radix_decode_key - Decodes a radix key back into a physical address and order.
 * @key: The unsigned long key to decode.
 * @order: An output parameter, a pointer to an unsigned int where the decoded
 *         page order will be stored.
 *
 * This function reverses the encoding performed by kho_radix_encode_key(),
 * extracting the original physical address and page order from a given key.
 *
 * Return: The decoded physical address.
 */
static phys_addr_t kho_radix_decode_key(unsigned long key, unsigned int *order)
{
	unsigned int order_bit = fls64(key);
	phys_addr_t phys;

	/* order_bit is numbered starting at 1 from fls64 */
	*order = KHO_ORDER_0_LOG2 - order_bit + 1;
	/* The order is discarded by the shift */
	phys = key << (PAGE_SHIFT + *order);

	return phys;
}

static unsigned long kho_radix_get_bitmap_index(unsigned long key)
{
	return key % (1 << KHO_BITMAP_SIZE_LOG2);
}

static unsigned long kho_radix_get_table_index(unsigned long key,
					       unsigned int level)
{
	int s;

	s = ((level - 1) * KHO_TABLE_SIZE_LOG2) + KHO_BITMAP_SIZE_LOG2;
	return (key >> s) % (1 << KHO_TABLE_SIZE_LOG2);
}

/**
 * kho_radix_add_page - Marks a page as preserved in the radix tree.
 * @tree: The KHO radix tree.
 * @pfn: The page frame number of the page to preserve.
 * @order: The order of the page.
 *
 * This function traverses the radix tree based on the key derived from @pfn
 * and @order. It sets the corresponding bit in the leaf bitmap to mark the
 * page for preservation. If intermediate nodes do not exist along the path,
 * they are allocated and added to the tree.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int kho_radix_add_page(struct kho_radix_tree *tree,
		       unsigned long pfn, unsigned int order)
{
	/* Newly allocated nodes for error cleanup */
	struct kho_radix_node *intermediate_nodes[KHO_TREE_MAX_DEPTH] = { 0 };
	unsigned long key = kho_radix_encode_key(PFN_PHYS(pfn), order);
	struct kho_radix_node *anchor_node = NULL;
	struct kho_radix_node *node = tree->root;
	struct kho_radix_node *new_node;
	unsigned int i, idx, anchor_idx;
	struct kho_radix_leaf *leaf;
	int err = 0;

	if (WARN_ON_ONCE(!tree->root))
		return -EINVAL;

	might_sleep();

	guard(mutex)(&tree->lock);

	/* Go from high levels to low levels */
	for (i = KHO_TREE_MAX_DEPTH - 1; i > 0; i--) {
		idx = kho_radix_get_table_index(key, i);

		if (node->table[idx]) {
			node = phys_to_virt(node->table[idx]);
			continue;
		}

		/* Next node is empty, create a new node for it */
		new_node = (struct kho_radix_node *)get_zeroed_page(GFP_KERNEL);
		if (!new_node) {
			err = -ENOMEM;
			goto err_free_nodes;
		}

		node->table[idx] = virt_to_phys(new_node);

		/*
		 * Capture the node where the new branch starts for cleanup
		 * if allocation fails.
		 */
		if (!anchor_node) {
			anchor_node = node;
			anchor_idx = idx;
		}
		intermediate_nodes[i] = new_node;

		node = new_node;
	}

	/* Handle the leaf level bitmap (level 0) */
	idx = kho_radix_get_bitmap_index(key);
	leaf = (struct kho_radix_leaf *)node;
	__set_bit(idx, leaf->bitmap);

	return 0;

err_free_nodes:
	for (i = KHO_TREE_MAX_DEPTH - 1; i > 0; i--) {
		if (intermediate_nodes[i])
			free_page((unsigned long)intermediate_nodes[i]);
	}
	if (anchor_node)
		anchor_node->table[anchor_idx] = 0;

	return err;
}
EXPORT_SYMBOL_GPL(kho_radix_add_page);

/**
 * kho_radix_del_page - Removes a page's preservation status from the radix tree.
 * @tree: The KHO radix tree.
 * @pfn: The page frame number of the page to unpreserve.
 * @order: The order of the page.
 *
 * This function traverses the radix tree and clears the bit corresponding to
 * the page, effectively removing its "preserved" status. It does not free
 * the tree's intermediate nodes, even if they become empty.
 */
void kho_radix_del_page(struct kho_radix_tree *tree, unsigned long pfn,
			unsigned int order)
{
	unsigned long key = kho_radix_encode_key(PFN_PHYS(pfn), order);
	struct kho_radix_node *node = tree->root;
	struct kho_radix_leaf *leaf;
	unsigned int i, idx;

	if (WARN_ON_ONCE(!tree->root))
		return;

	might_sleep();

	guard(mutex)(&tree->lock);

	/* Go from high levels to low levels */
	for (i = KHO_TREE_MAX_DEPTH - 1; i > 0; i--) {
		idx = kho_radix_get_table_index(key, i);

		/*
		 * Attempting to delete a page that has not been preserved,
		 * return with a warning.
		 */
		if (WARN_ON(!node->table[idx]))
			return;

		node = phys_to_virt(node->table[idx]);
	}

	/* Handle the leaf level bitmap (level 0) */
	leaf = (struct kho_radix_leaf *)node;
	idx = kho_radix_get_bitmap_index(key);
	__clear_bit(idx, leaf->bitmap);
}
EXPORT_SYMBOL_GPL(kho_radix_del_page);

static int kho_radix_walk_leaf(struct kho_radix_leaf *leaf,
			       unsigned long key,
			       kho_radix_tree_walk_callback_t cb)
{
	unsigned long *bitmap = (unsigned long *)leaf;
	unsigned int order;
	phys_addr_t phys;
	unsigned int i;
	int err;

	for_each_set_bit(i, bitmap, PAGE_SIZE * BITS_PER_BYTE) {
		phys = kho_radix_decode_key(key | i, &order);
		err = cb(phys, order);
		if (err)
			return err;
	}

	return 0;
}

static int __kho_radix_walk_tree(struct kho_radix_node *root,
				 unsigned int level, unsigned long start,
				 kho_radix_tree_walk_callback_t cb)
{
	struct kho_radix_node *node;
	struct kho_radix_leaf *leaf;
	unsigned long key, i;
	unsigned int shift;
	int err;

	for (i = 0; i < PAGE_SIZE / sizeof(phys_addr_t); i++) {
		if (!root->table[i])
			continue;

		shift = ((level - 1) * KHO_TABLE_SIZE_LOG2) +
			KHO_BITMAP_SIZE_LOG2;
		key = start | (i << shift);

		node = phys_to_virt(root->table[i]);

		if (level == 1) {
			/*
			 * we are at level 1,
			 * node is pointing to the level 0 bitmap.
			 */
			leaf = (struct kho_radix_leaf *)node;
			err = kho_radix_walk_leaf(leaf, key, cb);
		} else {
			err  = __kho_radix_walk_tree(node, level - 1,
						     key, cb);
		}

		if (err)
			return err;
	}

	return 0;
}

/**
 * kho_radix_walk_tree - Traverses the radix tree and calls a callback for each preserved page.
 * @tree: A pointer to the KHO radix tree to walk.
 * @cb: A callback function of type kho_radix_tree_walk_callback_t that will be
 *      invoked for each preserved page found in the tree. The callback receives
 *      the physical address and order of the preserved page.
 *
 * This function walks the radix tree, searching from the specified top level
 * down to the lowest level (level 0). For each preserved page found, it invokes
 * the provided callback, passing the page's physical address and order.
 *
 * Return: 0 if the walk completed the specified tree, or the non-zero return
 *         value from the callback that stopped the walk.
 */
int kho_radix_walk_tree(struct kho_radix_tree *tree,
			kho_radix_tree_walk_callback_t cb)
{
	if (WARN_ON_ONCE(!tree->root))
		return -EINVAL;

	guard(mutex)(&tree->lock);

	return __kho_radix_walk_tree(tree->root, KHO_TREE_MAX_DEPTH - 1, 0, cb);
}
EXPORT_SYMBOL_GPL(kho_radix_walk_tree);
