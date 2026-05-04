// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <kunit/test-bug.h>

#include <linux/export.h>
#include <linux/kmemleak.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include <linux/gpu_buddy.h>
#include <drm/drm_buddy.h>
#include <drm/drm_print.h>

/**
 * drm_buddy_block_print - print block information
 *
 * @mm: DRM buddy manager
 * @block: DRM buddy block
 * @p: DRM printer to use
 */
void drm_buddy_block_print(struct gpu_buddy *mm,
			   struct gpu_buddy_block *block,
			   struct drm_printer *p)
{
	u64 start = gpu_buddy_block_offset(block);
	u64 size = gpu_buddy_block_size(mm, block);

	drm_printf(p, "%#018llx-%#018llx: %llu\n", start, start + size, size);
}
EXPORT_SYMBOL(drm_buddy_block_print);

/**
 * drm_buddy_print - print allocator state
 *
 * @mm: DRM buddy manager
 * @p: DRM printer to use
 */
void drm_buddy_print(struct gpu_buddy *mm, struct drm_printer *p)
{
	u64 *clear_count;
	int order;
	unsigned int i;

	drm_printf(p, "chunk_size: %lluKiB, total: %lluMiB, free: %lluMiB, clear_free: %lluMiB\n",
		   mm->chunk_size >> 10, mm->size >> 20, mm->avail >> 20, mm->clear_avail >> 20);

	clear_count = kcalloc(mm->max_order + 1, sizeof(*clear_count), GFP_KERNEL);

	if (clear_count) {
		LIST_HEAD(dfs);

		for (i = 0; i < mm->n_roots; i++)
			list_add_tail(&mm->roots[i]->tmp_link, &dfs);

		while (!list_empty(&dfs)) {
			struct gpu_buddy_block *block =
				list_first_entry(&dfs, struct gpu_buddy_block, tmp_link);

			list_del(&block->tmp_link);

			if ((block->header & GPU_BUDDY_HEADER_STATE) == GPU_BUDDY_SPLIT) {
				list_add(&block->right->tmp_link, &dfs);
				list_add(&block->left->tmp_link, &dfs);
			} else if (gpu_buddy_block_is_free(block) &&
				   gpu_buddy_block_is_clear(block)) {
				clear_count[gpu_buddy_block_order(block)]++;
			}
		}
	}

	for (order = mm->max_order; order >= 0; order--) {
		struct gpu_buddy_block *block, *tmp;
		struct rb_root *root;
		u64 count = 0, free;

		root = &mm->free_tree[order];
		rbtree_postorder_for_each_entry_safe(block, tmp, root, rb) {
			BUG_ON(!gpu_buddy_block_is_free(block));
			count++;
		}

		if (clear_count)
			count += clear_count[order];

		drm_printf(p, "order-%2d ", order);

		free = count * (mm->chunk_size << order);
		if (free < SZ_1M)
			drm_printf(p, "free: %8llu KiB", free >> 10);
		else
			drm_printf(p, "free: %8llu MiB", free >> 20);

		drm_printf(p, ", blocks: %llu\n", count);
	}

	kfree(clear_count);
}
EXPORT_SYMBOL(drm_buddy_print);

MODULE_DESCRIPTION("DRM-specific GPU Buddy Allocator Print Helpers");
MODULE_LICENSE("Dual MIT/GPL");
