// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <linux/bug.h>
#include <linux/export.h>
#include <linux/kmemleak.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include <linux/gpu_buddy.h>

/**
 * gpu_buddy_assert - assert a condition in the buddy allocator
 * @condition: condition expected to be true
 *
 * When CONFIG_KUNIT is enabled, evaluates @condition and, if false, triggers
 * a WARN_ON() and also calls kunit_fail_current_test() so that any running
 * kunit test is properly marked as failed. The stringified condition is
 * included in the failure message for easy identification.
 *
 * When CONFIG_KUNIT is not enabled, this reduces to WARN_ON() so production
 * builds retain the same warning semantics as before.
 */
#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test-bug.h>
#define gpu_buddy_assert(condition) do {						\
	if (WARN_ON(!(condition)))						\
		kunit_fail_current_test("gpu_buddy_assert(" #condition ")");	\
} while (0)
#else
#define gpu_buddy_assert(condition) WARN_ON(!(condition))
#endif

static struct kmem_cache *slab_blocks;

static struct kmem_cache *slab_extents;

/*
 * Clear tracker
 * -------------
 *
 * The clear tracker maintains an augmented interval rbtree of contiguous
 * cleared (zeroed) address ranges, decoupled from the buddy free trees.
 * Each node covers a maximal coalesced run; adjacent extents are merged
 * on insertion so the tree always holds the smallest possible number of
 * extents.  The augmentation field @subtree_max_size lets the allocator
 * locate the largest cleared extent in O(log E).
 */

static u64 extent_size(struct gpu_clear_extent *clear_extent)
{
	return clear_extent->end - clear_extent->start;
}

RB_DECLARE_CALLBACKS_MAX(static, gpu_clear_augment_cb,
			 struct gpu_clear_extent, rb,
			 u64, subtree_max_size,
			 extent_size)

static struct gpu_clear_extent *extent_alloc(void)
{
	return kmem_cache_zalloc(slab_extents, GFP_KERNEL);
}

static void extent_free(struct gpu_clear_extent *clear_extent)
{
	kmem_cache_free(slab_extents, clear_extent);
}

/* Return the rightmost extent whose start is strictly below @offset. */
static struct gpu_clear_extent *
prev_extent(struct gpu_clear_tracker *clear_tracker, u64 offset)
{
	struct rb_node *rb = clear_tracker->root.rb_node;
	struct gpu_clear_extent *clear_extent = NULL;

	while (rb) {
		struct gpu_clear_extent *tmp_extent =
			rb_entry(rb, struct gpu_clear_extent, rb);

		if (tmp_extent->start < offset) {
			clear_extent = tmp_extent;
			rb = rb->rb_right;
		} else {
			rb = rb->rb_left;
		}
	}

	return clear_extent;
}

/* Return the leftmost extent whose start is at or above @offset. */
static struct gpu_clear_extent *
next_extent(struct gpu_clear_tracker *clear_tracker, u64 offset)
{
	struct rb_node *rb = clear_tracker->root.rb_node;
	struct gpu_clear_extent *clear_extent = NULL;

	while (rb) {
		struct gpu_clear_extent *tmp_extent =
			rb_entry(rb, struct gpu_clear_extent, rb);

		if (tmp_extent->start >= offset) {
			clear_extent = tmp_extent;
			rb = rb->rb_left;
		} else {
			rb = rb->rb_right;
		}
	}

	return clear_extent;
}

static void insert_extent(struct gpu_clear_tracker *clear_tracker,
			  struct gpu_clear_extent *clear_extent)
{
	struct rb_node **link = &clear_tracker->root.rb_node;
	struct rb_node *parent = NULL;
	u64 size = extent_size(clear_extent);

	while (*link) {
		struct gpu_clear_extent *tmp_extent;

		parent = *link;
		tmp_extent = rb_entry(parent, struct gpu_clear_extent, rb);

		if (tmp_extent->subtree_max_size < size)
			tmp_extent->subtree_max_size = size;

		if (clear_extent->start < tmp_extent->start)
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}

	clear_extent->subtree_max_size = size;
	rb_link_node(&clear_extent->rb, parent, link);
	rb_insert_augmented(&clear_extent->rb, &clear_tracker->root, &gpu_clear_augment_cb);
}

static void remove_extent(struct gpu_clear_tracker *clear_tracker,
			  struct gpu_clear_extent *clear_extent)
{
	rb_erase_augmented(&clear_extent->rb, &clear_tracker->root, &gpu_clear_augment_cb);
	RB_CLEAR_NODE(&clear_extent->rb);
}

static void gpu_clear_tracker_init(struct gpu_clear_tracker *clear_tracker)
{
	clear_tracker->root = RB_ROOT;
	clear_tracker->total_clear = 0;
}

static void gpu_clear_tracker_fini(struct gpu_clear_tracker *clear_tracker)
{
	struct rb_node *rb;

	while ((rb = rb_first(&clear_tracker->root))) {
		struct gpu_clear_extent *clear_extent =
			rb_entry(rb, struct gpu_clear_extent, rb);

		remove_extent(clear_tracker, clear_extent);
		extent_free(clear_extent);
	}

	clear_tracker->total_clear = 0;
}

/*
 * Mark the range [start, start + size] as cleared. Merge with the neighbour on
 * each side if they are contiguous, so the tree never holds two adjacent ranges.
 */
static void gpu_clear_tracker_mark_clear(struct gpu_clear_tracker *clear_tracker,
					 u64 start, u64 size)
{
	struct gpu_clear_extent *left, *right, *clear_extent;
	u64 end = start + size;

	if (!size)
		return;

	/* Find contiguous neighbours, if any. */
	left = prev_extent(clear_tracker, start);
	if (left && left->end != start)
		left = NULL;

	right = next_extent(clear_tracker, end);
	if (right && right->start != end)
		right = NULL;

	if (left && right) {
		/* Merge left + new + right into a single extent. */
		remove_extent(clear_tracker, left);
		remove_extent(clear_tracker, right);
		left->end = right->end;
		extent_free(right);
		insert_extent(clear_tracker, left);
	} else if (left) {
		/* Extend left neighbour rightwards. */
		remove_extent(clear_tracker, left);
		left->end = end;
		insert_extent(clear_tracker, left);
	} else if (right) {
		/* Extend right neighbour leftwards. */
		remove_extent(clear_tracker, right);
		right->start = start;
		insert_extent(clear_tracker, right);
	} else {
		/* Standalone extent. */
		clear_extent = extent_alloc();
		if (!clear_extent)
			return;

		clear_extent->start = start;
		clear_extent->end   = end;
		insert_extent(clear_tracker, clear_extent);
	}

	clear_tracker->total_clear += size;
}

/*
 * Mark the range [start, start + size] as dirty. Remove the range from every
 * overlapping clear extent, splitting one extent in two if the dirty range
 * falls strictly inside it.
 */
static void gpu_clear_tracker_mark_dirty(struct gpu_clear_tracker *clear_tracker,
					 u64 start, u64 size)
{
	struct gpu_clear_extent *clear_extent, *next;
	u64 end = start + size;

	if (!size)
		return;

	clear_extent = prev_extent(clear_tracker, start + 1);
	if (!clear_extent)
		clear_extent = next_extent(clear_tracker, start);

	while (clear_extent && clear_extent->start < end) {
		struct rb_node *next_node = rb_next(&clear_extent->rb);
		u64 extent_start = clear_extent->start;
		u64 extent_end = clear_extent->end;

		if (next_node)
			next = rb_entry(next_node, struct gpu_clear_extent, rb);
		else
			next = NULL;

		/* Skip a non-overlapping neighbour returned by prev_extent(). */
		if (extent_end <= start) {
			clear_extent = next;
			continue;
		}

		if (extent_start < start && extent_end > end) {
			/* Dirty range falls strictly inside: split into left + right. */
			struct gpu_clear_extent *right = extent_alloc();

			if (!right) {
				remove_extent(clear_tracker, clear_extent);
				extent_free(clear_extent);

				clear_tracker->total_clear -=
					(extent_end - extent_start);

				clear_extent = next;
				continue;
			}

			remove_extent(clear_tracker, clear_extent);

			clear_extent->end = start;
			right->start = end;
			right->end   = extent_end;

			insert_extent(clear_tracker, clear_extent);
			insert_extent(clear_tracker, right);

			clear_tracker->total_clear -= size;
		} else if (extent_start >= start && extent_end <= end) {
			/* Extent fully covered: drop it. */
			remove_extent(clear_tracker, clear_extent);
			extent_free(clear_extent);

			clear_tracker->total_clear -= (extent_end - extent_start);
		} else if (extent_start < start) {
			/* Extent overlaps from the left: trim its right end. */
			remove_extent(clear_tracker, clear_extent);
			clear_extent->end = start;
			insert_extent(clear_tracker, clear_extent);

			clear_tracker->total_clear -= (extent_end - start);
		} else {
			/* Extent overlaps from the right: trim its left end. */
			remove_extent(clear_tracker, clear_extent);
			clear_extent->start = end;
			insert_extent(clear_tracker, clear_extent);

			clear_tracker->total_clear -= (end - extent_start);
		}

		clear_extent = next;
	}
}

/*
 * Returns true if the range [start, start + size] lies entirely within
 * a single clear extent in the tracker, i.e. the whole range is known
 * to be cleared.
 */
static bool gpu_clear_tracker_is_clear(struct gpu_clear_tracker *clear_tracker,
				       u64 start, u64 size)
{
	struct gpu_clear_extent *clear_extent;
	u64 end = start + size;

	clear_extent = prev_extent(clear_tracker, start + 1);
	if (!clear_extent)
		return false;

	return clear_extent->start <= start && clear_extent->end >= end;
}

static struct rb_node *
clear_tracker_descend_right(struct rb_node *node, u64 min_size)
{
	while (node->rb_right) {
		struct gpu_clear_extent *tmp_extent;

		tmp_extent = rb_entry(node->rb_right, struct gpu_clear_extent, rb);

		if (tmp_extent->subtree_max_size < min_size)
			break;
		node = node->rb_right;
	}

	return node;
}

static struct gpu_clear_extent *
gpu_clear_tracker_find(struct gpu_clear_tracker *clear_tracker, u64 min_size)
{
	struct rb_node *rb = clear_tracker->root.rb_node;
	struct gpu_clear_extent *root_extent;
	struct rb_node *parent;

	if (WARN_ON(!min_size || !is_power_of_2(min_size)))
		return NULL;

	if (!rb)
		return NULL;

	root_extent = rb_entry(rb, struct gpu_clear_extent, rb);
	if (root_extent->subtree_max_size < min_size)
		return NULL;

	rb = clear_tracker_descend_right(rb, min_size);

	while (rb) {
		struct gpu_clear_extent *clear_extent;
		u64 aligned_start;

		clear_extent = rb_entry(rb, struct gpu_clear_extent, rb);
		aligned_start = ALIGN(clear_extent->start, min_size);

		/* Check if a naturally aligned min_size block fits. */
		if (aligned_start <= clear_extent->end &&
		    clear_extent->end - aligned_start >= min_size)
			return clear_extent;

		if (rb->rb_left) {
			struct gpu_clear_extent *tmp_extent;

			tmp_extent = rb_entry(rb->rb_left, struct gpu_clear_extent, rb);
			if (tmp_extent->subtree_max_size >= min_size) {
				rb = clear_tracker_descend_right(rb->rb_left, min_size);
				continue;
			}
		}

		/* Walk up until we exit a node via its right child. */
		parent = rb_parent(rb);
		while (parent && parent->rb_right != rb) {
			rb = parent;
			parent = rb_parent(rb);
		}
		rb = parent;
	}

	return NULL;
}

static unsigned int
gpu_buddy_block_state(struct gpu_buddy_block *block)
{
	return block->header & GPU_BUDDY_HEADER_STATE;
}

static bool
gpu_buddy_block_is_allocated(struct gpu_buddy_block *block)
{
	return gpu_buddy_block_state(block) == GPU_BUDDY_ALLOCATED;
}

static bool
gpu_buddy_block_is_split(struct gpu_buddy_block *block)
{
	return gpu_buddy_block_state(block) == GPU_BUDDY_SPLIT;
}

static unsigned int gpu_buddy_block_offset_alignment(struct gpu_buddy_block *block)
{
	u64 offset = gpu_buddy_block_offset(block);

	if (!offset)
		/*
		 * __ffs64(0) is undefined; offset 0 is maximally aligned, so return
		 * a value greater than any possible alignment.
		 */
		return 64 + 1;

	return __ffs64(offset);
}

static inline bool
gpu_buddy_block_is_dirty(struct gpu_buddy_block *block)
{
	return !gpu_buddy_block_is_clear(block);
}

static inline void gpu_buddy_augment_compute(struct gpu_buddy_block *block)
{
	struct gpu_buddy_block *right;
	struct gpu_buddy_block *left;
	unsigned int max_align;
	bool has_dirty;

	max_align = gpu_buddy_block_offset_alignment(block);
	has_dirty = gpu_buddy_block_is_dirty(block);

	left = rb_entry_safe(block->rb.rb_left, struct gpu_buddy_block, rb);
	if (left) {
		if (left->subtree_max_alignment > max_align)
			max_align = left->subtree_max_alignment;

		has_dirty |= left->subtree_has_dirty;
	}

	right = rb_entry_safe(block->rb.rb_right, struct gpu_buddy_block, rb);
	if (right) {
		if (right->subtree_max_alignment > max_align)
			max_align = right->subtree_max_alignment;

		has_dirty |= right->subtree_has_dirty;
	}

	block->subtree_max_alignment = max_align;
	block->subtree_has_dirty = has_dirty;
}

static void gpu_buddy_augment_propagate(struct rb_node *rb, struct rb_node *stop)
{
	while (rb != stop) {
		struct gpu_buddy_block *block;
		unsigned int old_align;
		bool old_has_dirty;

		block = rb_entry(rb, struct gpu_buddy_block, rb);
		old_align = block->subtree_max_alignment;
		old_has_dirty = block->subtree_has_dirty;

		gpu_buddy_augment_compute(block);
		if (block->subtree_max_alignment == old_align &&
		    block->subtree_has_dirty == old_has_dirty)
			break;

		rb = rb_parent(&block->rb);
	}
}

static void gpu_buddy_augment_copy(struct rb_node *rb_old, struct rb_node *rb_new)
{
	struct gpu_buddy_block *old;
	struct gpu_buddy_block *new;

	old = rb_entry(rb_old, struct gpu_buddy_block, rb);
	new = rb_entry(rb_new, struct gpu_buddy_block, rb);

	new->subtree_max_alignment = old->subtree_max_alignment;
	new->subtree_has_dirty = old->subtree_has_dirty;
}

static void gpu_buddy_augment_rotate(struct rb_node *rb_old, struct rb_node *rb_new)
{
	struct gpu_buddy_block *old;
	struct gpu_buddy_block *new;

	old = rb_entry(rb_old, struct gpu_buddy_block, rb);
	new = rb_entry(rb_new, struct gpu_buddy_block, rb);

	new->subtree_max_alignment = old->subtree_max_alignment;
	new->subtree_has_dirty = old->subtree_has_dirty;

	gpu_buddy_augment_compute(old);
}

static const struct rb_augment_callbacks gpu_buddy_augment_cb = {
	.propagate = gpu_buddy_augment_propagate,
	.copy      = gpu_buddy_augment_copy,
	.rotate    = gpu_buddy_augment_rotate,
};

static struct gpu_buddy_block *gpu_block_alloc(struct gpu_buddy *mm,
					       struct gpu_buddy_block *parent,
					       unsigned int order,
					       u64 offset)
{
	struct gpu_buddy_block *block;

	BUG_ON(order > GPU_BUDDY_MAX_ORDER);

	block = kmem_cache_zalloc(slab_blocks, GFP_KERNEL);
	if (!block)
		return NULL;

	block->header = offset;
	block->header |= order;
	block->parent = parent;

	RB_CLEAR_NODE(&block->rb);

	BUG_ON(block->header & GPU_BUDDY_HEADER_UNUSED);
	return block;
}

static void gpu_block_free(struct gpu_buddy *mm,
			   struct gpu_buddy_block *block)
{
	kmem_cache_free(slab_blocks, block);
}

static struct gpu_buddy_block *
rbtree_get_free_block(const struct rb_node *node)
{
	return node ? rb_entry(node, struct gpu_buddy_block, rb) : NULL;
}

static struct gpu_buddy_block *
rbtree_last_free_block(struct rb_root *root)
{
	return rbtree_get_free_block(rb_last(root));
}

/*
 * Find the rightmost (highest-offset) free block in @root that is itself
 * dirty, by descending the tree using the subtree_has_dirty augment to
 * skip subtrees that contain only cleared blocks.  Returns NULL if no
 * dirty block exists in the tree.
 */
static struct gpu_buddy_block *
rbtree_last_dirty_free_block(struct rb_root *root)
{
	struct gpu_buddy_block *block = NULL;
	struct rb_node *node = root->rb_node;

	while (node) {
		struct gpu_buddy_block *right_block;
		struct gpu_buddy_block *node_block;

		node_block = rbtree_get_free_block(node);
		right_block = rbtree_get_free_block(node->rb_right);

		/*
		 * Prefer the rightmost subtree that contains a dirty block;
		 * fall back to the current node if it is itself dirty;
		 * otherwise descend left.
		 */
		if (right_block && right_block->subtree_has_dirty) {
			node = node->rb_right;
			continue;
		}

		if (gpu_buddy_block_is_dirty(node_block)) {
			block = node_block;
			break;
		}

		node = node->rb_left;
	}

	return block;
}

static void rbtree_insert(struct gpu_buddy *mm,
			  struct gpu_buddy_block *block)
{
	struct rb_node **link, *parent = NULL;
	struct gpu_buddy_block *node;
	unsigned int block_alignment;
	struct rb_root *root;
	unsigned int order;
	bool block_dirty;

	order = gpu_buddy_block_order(block);
	block_alignment = gpu_buddy_block_offset_alignment(block);
	block_dirty = gpu_buddy_block_is_dirty(block);

	root = &mm->free_tree[order];
	link = &root->rb_node;

	while (*link) {
		parent = *link;
		node = rbtree_get_free_block(parent);
		/*
		 * Manual augmentation update during insertion traversal. Required
		 * because rb_insert_augmented() only calls rotate callback during
		 * rotations. This ensures all ancestors on the insertion path have
		 * correct subtree_max_alignment / subtree_has_dirty values.
		 */
		if (node->subtree_max_alignment < block_alignment)
			node->subtree_max_alignment = block_alignment;
		if (block_dirty)
			node->subtree_has_dirty = true;

		if (gpu_buddy_block_offset(block) < gpu_buddy_block_offset(node))
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}

	block->subtree_max_alignment = block_alignment;
	block->subtree_has_dirty = block_dirty;
	rb_link_node(&block->rb, parent, link);
	rb_insert_augmented(&block->rb, root, &gpu_buddy_augment_cb);
}

static void rbtree_remove(struct gpu_buddy *mm,
			  struct gpu_buddy_block *block)
{
	unsigned int order = gpu_buddy_block_order(block);

	rb_erase_augmented(&block->rb, &mm->free_tree[order], &gpu_buddy_augment_cb);
	RB_CLEAR_NODE(&block->rb);
}

static void mark_allocated(struct gpu_buddy *mm,
			   struct gpu_buddy_block *block)
{
	block->header &= ~GPU_BUDDY_HEADER_STATE;
	block->header |= GPU_BUDDY_ALLOCATED;

	rbtree_remove(mm, block);
}

static void mark_free(struct gpu_buddy *mm,
		      struct gpu_buddy_block *block)
{
	block->header &= ~GPU_BUDDY_HEADER_STATE;
	block->header |= GPU_BUDDY_FREE;

	if (gpu_clear_tracker_is_clear(&mm->clear,
				       gpu_buddy_block_offset(block),
				       gpu_buddy_block_size(mm, block)))
		block->header |= GPU_BUDDY_HEADER_CLEAR;
	else
		block->header &= ~GPU_BUDDY_HEADER_CLEAR;

	rbtree_insert(mm, block);
}

static void mark_split(struct gpu_buddy *mm,
		       struct gpu_buddy_block *block)
{
	block->header &= ~GPU_BUDDY_HEADER_STATE;
	block->header |= GPU_BUDDY_SPLIT;

	rbtree_remove(mm, block);
}

static inline bool overlaps(u64 s1, u64 e1, u64 s2, u64 e2)
{
	return s1 <= e2 && e1 >= s2;
}

static inline bool contains(u64 s1, u64 e1, u64 s2, u64 e2)
{
	return s1 <= s2 && e1 >= e2;
}

static struct gpu_buddy_block *
__get_buddy(struct gpu_buddy_block *block)
{
	struct gpu_buddy_block *parent;

	parent = block->parent;
	if (!parent)
		return NULL;

	if (parent->left == block)
		return parent->right;

	return parent->left;
}

static unsigned int __gpu_buddy_free(struct gpu_buddy *mm,
				     struct gpu_buddy_block *block)
{
	struct gpu_buddy_block *parent;
	unsigned int order;

	while ((parent = block->parent)) {
		struct gpu_buddy_block *buddy = __get_buddy(block);

		if (!gpu_buddy_block_is_free(buddy))
			break;

		rbtree_remove(mm, buddy);

		gpu_block_free(mm, block);
		gpu_block_free(mm, buddy);

		block = parent;
	}

	order = gpu_buddy_block_order(block);
	mark_free(mm, block);

	return order;
}

static void undo_partial_split(struct gpu_buddy *mm,
			       struct gpu_buddy_block *block)
{
	struct gpu_buddy_block *buddy = __get_buddy(block);

	if (buddy &&
	    gpu_buddy_block_is_free(block) &&
	    gpu_buddy_block_is_free(buddy))
		__gpu_buddy_free(mm, block);
}

/**
 * gpu_buddy_init - init memory manager
 *
 * @mm: GPU buddy manager to initialize
 * @size: size in bytes to manage
 * @chunk_size: minimum page size in bytes for our allocations
 *
 * Initializes the memory manager and its resources.
 *
 * Returns:
 * 0 on success, error code on failure.
 */
int gpu_buddy_init(struct gpu_buddy *mm, u64 size, u64 chunk_size)
{
	unsigned int root_count = 0;
	u64 offset = 0;

	if (size < chunk_size)
		return -EINVAL;

	if (chunk_size < SZ_4K)
		return -EINVAL;

	if (!is_power_of_2(chunk_size))
		return -EINVAL;

	size = round_down(size, chunk_size);

	mm->size = size;
	mm->avail = size;
	mm->clear_avail = 0;
	mm->chunk_size = chunk_size;
	mm->max_order = ilog2(size) - ilog2(chunk_size);

	BUG_ON(mm->max_order > GPU_BUDDY_MAX_ORDER);

	mm->free_tree = kcalloc(mm->max_order + 1,
				sizeof(struct rb_root),
				GFP_KERNEL);
	if (!mm->free_tree)
		return -ENOMEM;

	gpu_clear_tracker_init(&mm->clear);

	mm->n_roots = hweight64(size);

	mm->roots = kmalloc_array(mm->n_roots,
				  sizeof(struct gpu_buddy_block *),
				  GFP_KERNEL);
	if (!mm->roots)
		goto out_free_tree;

	/*
	 * Split into power-of-two blocks, in case we are given a size that is
	 * not itself a power-of-two.
	 */
	do {
		struct gpu_buddy_block *root;
		unsigned int order;
		u64 root_size;

		order = ilog2(size) - ilog2(chunk_size);
		root_size = chunk_size << order;

		root = gpu_block_alloc(mm, NULL, order, offset);
		if (!root)
			goto out_free_roots;

		mark_free(mm, root);

		BUG_ON(root_count > mm->max_order);
		BUG_ON(gpu_buddy_block_size(mm, root) < chunk_size);

		mm->roots[root_count] = root;

		offset += root_size;
		size -= root_size;
		root_count++;
	} while (size);

#ifdef CONFIG_LOCKDEP
	mm->lock_dep_map = NULL;
#endif
	return 0;

out_free_roots:
	while (root_count--)
		gpu_block_free(mm, mm->roots[root_count]);
	kfree(mm->roots);
out_free_tree:
	gpu_clear_tracker_fini(&mm->clear);
	kfree(mm->free_tree);
	return -ENOMEM;
}
EXPORT_SYMBOL(gpu_buddy_init);

/**
 * gpu_buddy_fini - tear down the memory manager
 *
 * @mm: GPU buddy manager to free
 *
 * Cleanup memory manager resources and the freetree
 */
void gpu_buddy_fini(struct gpu_buddy *mm)
{
	u64 root_size, size;
	unsigned int order;
	int i;

	size = mm->size;

	for (i = 0; i < mm->n_roots; ++i) {
		order = ilog2(size) - ilog2(mm->chunk_size);
		root_size = mm->chunk_size << order;

		gpu_buddy_assert(gpu_buddy_block_is_free(mm->roots[i]));
		gpu_block_free(mm, mm->roots[i]);
		size -= root_size;
	}

	gpu_buddy_assert(mm->avail == mm->size);

	gpu_clear_tracker_fini(&mm->clear);
	kfree(mm->free_tree);
	kfree(mm->roots);
}
EXPORT_SYMBOL(gpu_buddy_fini);

static int split_block(struct gpu_buddy *mm,
		       struct gpu_buddy_block *block)
{
	unsigned int block_order = gpu_buddy_block_order(block) - 1;
	u64 offset = gpu_buddy_block_offset(block);

	BUG_ON(!gpu_buddy_block_is_free(block));
	BUG_ON(!gpu_buddy_block_order(block));

	block->left = gpu_block_alloc(mm, block, block_order, offset);
	if (!block->left)
		return -ENOMEM;

	block->right = gpu_block_alloc(mm, block, block_order,
				       offset + (mm->chunk_size << block_order));
	if (!block->right) {
		gpu_block_free(mm, block->left);
		return -ENOMEM;
	}

	mark_split(mm, block);
	mark_free(mm, block->left);
	mark_free(mm, block->right);

	return 0;
}

/**
 * gpu_buddy_reset_clear - reset blocks clear state
 *
 * @mm: GPU buddy manager
 * @is_clear: blocks clear state
 *
 * Reset the clear state based on @is_clear value for each block
 * in the freetree.
 */
void gpu_buddy_reset_clear(struct gpu_buddy *mm, bool is_clear)
{
	unsigned int i;

	gpu_buddy_driver_lock_held(mm);

	gpu_clear_tracker_fini(&mm->clear);
	gpu_clear_tracker_init(&mm->clear);

	for (i = 0; i <= mm->max_order; ++i) {
		struct gpu_buddy_block *block, *tmp;

		rbtree_postorder_for_each_entry_safe(block, tmp,
						     &mm->free_tree[i], rb) {
			if (is_clear) {
				if (!gpu_buddy_block_is_clear(block))
					block->header |= GPU_BUDDY_HEADER_CLEAR;
				gpu_clear_tracker_mark_clear(&mm->clear,
							     gpu_buddy_block_offset(block),
							     gpu_buddy_block_size(mm, block));
			} else if (gpu_buddy_block_is_clear(block)) {
				block->header &= ~GPU_BUDDY_HEADER_CLEAR;
			}

			gpu_buddy_augment_compute(block);
		}
	}

	mm->clear_avail = mm->clear.total_clear;
}
EXPORT_SYMBOL(gpu_buddy_reset_clear);

/**
 * gpu_buddy_free_block - free a block
 *
 * @mm: GPU buddy manager
 * @block: block to be freed
 */
void gpu_buddy_free_block(struct gpu_buddy *mm,
			  struct gpu_buddy_block *block)
{
	bool was_clear = gpu_buddy_block_is_clear(block);
	u64 size   = gpu_buddy_block_size(mm, block);
	u64 offset = gpu_buddy_block_offset(block);

	gpu_buddy_driver_lock_held(mm);

	BUG_ON(!gpu_buddy_block_is_allocated(block));

	block->header &= ~GPU_BUDDY_HEADER_CLEAR;
	mm->avail += size;

	if (was_clear) {
		gpu_clear_tracker_mark_clear(&mm->clear, offset, size);
		mm->clear_avail = mm->clear.total_clear;
	}

	__gpu_buddy_free(mm, block);
}
EXPORT_SYMBOL(gpu_buddy_free_block);

static void __gpu_buddy_free_list(struct gpu_buddy *mm,
				  struct list_head *objects,
				  bool mark_clear,
				  bool mark_dirty)
{
	struct gpu_buddy_block *block, *on;

	gpu_buddy_assert(!(mark_dirty && mark_clear));

	list_for_each_entry_safe(block, on, objects, link) {
		/*
		 * Propagate the caller's clear/dirty intent onto the block header
		 * before handing it to gpu_buddy_free_block(), which will then
		 * update the clear tracker accordingly.
		 */
		if (mark_clear)
			block->header |= GPU_BUDDY_HEADER_CLEAR;
		else if (mark_dirty)
			block->header &= ~GPU_BUDDY_HEADER_CLEAR;
		gpu_buddy_free_block(mm, block);
		cond_resched();
	}
	INIT_LIST_HEAD(objects);
}

static void gpu_buddy_free_list_internal(struct gpu_buddy *mm,
					 struct list_head *objects)
{
	/*
	 * Don't touch the clear/dirty bit, since allocation is still internal
	 * at this point. For example we might have just failed part of the
	 * allocation.
	 */
	__gpu_buddy_free_list(mm, objects, false, false);
}

/**
 * gpu_buddy_free_list - free blocks
 *
 * @mm: GPU buddy manager
 * @objects: input list head to free blocks
 * @flags: optional flags like GPU_BUDDY_CLEARED
 */
void gpu_buddy_free_list(struct gpu_buddy *mm,
			 struct list_head *objects,
			 unsigned int flags)
{
	bool mark_clear = flags & GPU_BUDDY_CLEARED;

	gpu_buddy_driver_lock_held(mm);
	__gpu_buddy_free_list(mm, objects, mark_clear, !mark_clear);
}
EXPORT_SYMBOL(gpu_buddy_free_list);

static struct gpu_buddy_block *
__alloc_range_bias(struct gpu_buddy *mm,
		   u64 start, u64 end,
		   unsigned int order,
		   unsigned long flags)
{
	u64 req_size = mm->chunk_size << order;
	struct gpu_buddy_block *block;
	LIST_HEAD(dfs);
	int err;
	int i;

	end = end - 1;

	for (i = 0; i < mm->n_roots; ++i)
		list_add_tail(&mm->roots[i]->tmp_link, &dfs);

	do {
		u64 block_start;
		u64 block_end;

		block = list_first_entry_or_null(&dfs,
						 struct gpu_buddy_block,
						 tmp_link);
		if (!block)
			break;

		list_del(&block->tmp_link);

		if (gpu_buddy_block_order(block) < order)
			continue;

		block_start = gpu_buddy_block_offset(block);
		block_end = block_start + gpu_buddy_block_size(mm, block) - 1;

		if (!overlaps(start, end, block_start, block_end))
			continue;

		if (gpu_buddy_block_is_allocated(block))
			continue;

		if (block_start < start || block_end > end) {
			u64 adjusted_start = max(block_start, start);
			u64 adjusted_end = min(block_end, end);

			if (round_down(adjusted_end + 1, req_size) <=
			    round_up(adjusted_start, req_size))
				continue;
		}

		if (contains(start, end, block_start, block_end) &&
		    order == gpu_buddy_block_order(block)) {
			/*
			 * Find the free block within the range.
			 */
			if (gpu_buddy_block_is_free(block))
				return block;

			continue;
		}

		if (!gpu_buddy_block_is_split(block)) {
			err = split_block(mm, block);
			if (unlikely(err))
				goto err_undo;
		}

		list_add(&block->left->tmp_link, &dfs);
		list_add(&block->right->tmp_link, &dfs);
	} while (1);

	return ERR_PTR(-ENOSPC);

err_undo:
	undo_partial_split(mm, block);
	return ERR_PTR(err);
}

static struct gpu_buddy_block *
get_maxblock(struct gpu_buddy *mm,
	     unsigned int order,
	     unsigned long flags)
{
	struct gpu_buddy_block *max_block;
	struct gpu_buddy_block *block;
	bool prefer_clear;
	unsigned int i;

	max_block = NULL;
	prefer_clear = flags & GPU_BUDDY_CLEAR_ALLOCATION;

	for (i = order; i <= mm->max_order; ++i) {
		if (prefer_clear)
			block = rbtree_last_free_block(&mm->free_tree[i]);
		else
			block = rbtree_last_dirty_free_block(&mm->free_tree[i]);

		if (!block)
			continue;

		if (!max_block ||
		    gpu_buddy_block_offset(block) > gpu_buddy_block_offset(max_block))
			max_block = block;
	}

	if (max_block || prefer_clear)
		return max_block;

	for (i = order; i <= mm->max_order; ++i) {
		block = rbtree_last_free_block(&mm->free_tree[i]);
		if (!block)
			continue;

		if (!max_block ||
		    gpu_buddy_block_offset(block) > gpu_buddy_block_offset(max_block))
			max_block = block;
	}

	return max_block;
}

static struct gpu_buddy_block *
alloc_from_freetree(struct gpu_buddy *mm,
		    unsigned int order,
		    unsigned long flags)
{
	struct gpu_buddy_block *block = NULL;
	unsigned int tmp;
	int err;

	if (flags & GPU_BUDDY_TOPDOWN_ALLOCATION) {
		block = get_maxblock(mm, order, flags);
		if (block)
			tmp = gpu_buddy_block_order(block);
	} else if (!(flags & GPU_BUDDY_CLEAR_ALLOCATION)) {
		for (tmp = order; tmp <= mm->max_order; ++tmp) {
			block = rbtree_last_dirty_free_block(&mm->free_tree[tmp]);
			if (block)
				break;
		}
		if (!block) {
			for (tmp = order; tmp <= mm->max_order; ++tmp) {
				block = rbtree_last_free_block(&mm->free_tree[tmp]);
				if (block)
					break;
			}
		}
	} else {
		for (tmp = order; tmp <= mm->max_order; ++tmp) {
			block = rbtree_last_free_block(&mm->free_tree[tmp]);
			if (block)
				break;
		}
	}

	if (!block)
		return ERR_PTR(-ENOSPC);

	BUG_ON(!gpu_buddy_block_is_free(block));

	while (tmp != order) {
		err = split_block(mm, block);
		if (unlikely(err))
			goto err_undo;

		if (!(flags & GPU_BUDDY_CLEAR_ALLOCATION) &&
		    gpu_buddy_block_is_clear(block->right))
			block = block->left;
		else
			block = block->right;
		tmp--;
	}
	return block;

err_undo:
	if (tmp != order)
		__gpu_buddy_free(mm, block);
	return ERR_PTR(err);
}

static bool
gpu_buddy_can_offset_align(u64 size, u64 min_block_size)
{
	return size < min_block_size && is_power_of_2(size);
}

static bool gpu_buddy_subtree_can_satisfy(struct rb_node *node,
					  unsigned int alignment)
{
	struct gpu_buddy_block *block;

	block = rbtree_get_free_block(node);
	return block->subtree_max_alignment >= alignment;
}

static struct gpu_buddy_block *
gpu_buddy_find_block_aligned(struct gpu_buddy *mm,
			     unsigned int order,
			     unsigned int alignment,
			     unsigned long flags)
{
	struct rb_root *root = &mm->free_tree[order];
	struct rb_node *rb = root->rb_node;

	while (rb) {
		struct gpu_buddy_block *block = rbtree_get_free_block(rb);
		struct rb_node *left_node = rb->rb_left, *right_node = rb->rb_right;

		if (right_node) {
			if (gpu_buddy_subtree_can_satisfy(right_node, alignment)) {
				rb = right_node;
				continue;
			}
		}

		if (gpu_buddy_block_offset_alignment(block) >= alignment)
			return block;

		if (left_node) {
			if (gpu_buddy_subtree_can_satisfy(left_node, alignment)) {
				rb = left_node;
				continue;
			}
		}

		break;
	}

	return NULL;
}

static struct gpu_buddy_block *
gpu_buddy_offset_aligned_allocation(struct gpu_buddy *mm,
				    u64 size,
				    u64 min_block_size,
				    unsigned long flags)
{
	struct gpu_buddy_block *block = NULL;
	unsigned int order, tmp, alignment;
	unsigned long pages;
	int err;

	alignment = ilog2(min_block_size);
	pages = size >> ilog2(mm->chunk_size);
	order = fls(pages) - 1;

	for (tmp = order; tmp <= mm->max_order; ++tmp) {
		block = gpu_buddy_find_block_aligned(mm, tmp, alignment, flags);
		if (block)
			break;
	}

	if (!block)
		return ERR_PTR(-ENOSPC);

	while (gpu_buddy_block_order(block) > order) {
		struct gpu_buddy_block *left, *right;

		err = split_block(mm, block);
		if (unlikely(err))
			goto err_undo;

		left  = block->left;
		right = block->right;

		if (gpu_buddy_block_offset_alignment(right) >= alignment)
			block = right;
		else
			block = left;
	}

	return block;

err_undo:
	undo_partial_split(mm, block);
	return ERR_PTR(err);
}

static int __alloc_range(struct gpu_buddy *mm,
			 struct list_head *dfs,
			 u64 start, u64 size,
			 unsigned long flags,
			 struct list_head *blocks,
			 u64 *total_allocated_on_err)
{
	struct gpu_buddy_block *block;
	u64 total_allocated = 0;
	LIST_HEAD(allocated);
	u64 end;
	int err;

	end = start + size - 1;

	do {
		u64 block_start;
		u64 block_end;

		block = list_first_entry_or_null(dfs,
						 struct gpu_buddy_block,
						 tmp_link);
		if (!block)
			break;

		list_del(&block->tmp_link);

		block_start = gpu_buddy_block_offset(block);
		block_end = block_start + gpu_buddy_block_size(mm, block) - 1;

		if (!overlaps(start, end, block_start, block_end))
			continue;

		if (gpu_buddy_block_is_allocated(block)) {
			err = -ENOSPC;
			goto err_free;
		}

		if (contains(start, end, block_start, block_end)) {
			if (gpu_buddy_block_is_free(block)) {
				u64 bsize = gpu_buddy_block_size(mm, block);
				u64 boff  = gpu_buddy_block_offset(block);

				mark_allocated(mm, block);
				total_allocated += bsize;
				mm->avail -= bsize;

				block->header &= ~GPU_BUDDY_HEADER_CLEAR;
				if (gpu_clear_tracker_is_clear(&mm->clear,
							       boff, bsize)) {
					if (flags & GPU_BUDDY_CLEAR_ALLOCATION)
						block->header |= GPU_BUDDY_HEADER_CLEAR;
				}
				gpu_clear_tracker_mark_dirty(&mm->clear,
							     boff, bsize);
				mm->clear_avail = mm->clear.total_clear;

				list_add_tail(&block->link, &allocated);
				continue;
			}
		}

		if (!gpu_buddy_block_is_split(block)) {
			err = split_block(mm, block);
			if (unlikely(err))
				goto err_undo;
		}

		list_add(&block->right->tmp_link, dfs);
		list_add(&block->left->tmp_link, dfs);
	} while (1);

	if (total_allocated < size) {
		err = -ENOSPC;
		goto err_free;
	}

	list_splice_tail(&allocated, blocks);

	return 0;

err_undo:
	undo_partial_split(mm, block);

err_free:
	if (err == -ENOSPC && total_allocated_on_err) {
		list_splice_tail(&allocated, blocks);
		*total_allocated_on_err = total_allocated;
	} else {
		gpu_buddy_free_list_internal(mm, &allocated);
	}

	return err;
}

static int __gpu_buddy_alloc_range(struct gpu_buddy *mm,
				   u64 start,
				   u64 size,
				   unsigned long flags,
				   u64 *total_allocated_on_err,
				   struct list_head *blocks)
{
	LIST_HEAD(dfs);
	int i;

	for (i = 0; i < mm->n_roots; ++i)
		list_add_tail(&mm->roots[i]->tmp_link, &dfs);

	return __alloc_range(mm, &dfs, start, size, flags,
			     blocks, total_allocated_on_err);
}

static int __alloc_contig_try_harder(struct gpu_buddy *mm,
				     u64 size,
				     u64 min_block_size,
				     unsigned long flags,
				     struct list_head *blocks)
{
	u64 rhs_offset, lhs_offset, lhs_size, filled;
	struct gpu_buddy_block *block;
	LIST_HEAD(blocks_lhs);
	struct rb_root *root;
	struct rb_node *iter;
	unsigned long pages;
	unsigned int order;
	u64 modify_size;
	int err;

	modify_size = rounddown_pow_of_two(size);
	pages = modify_size >> ilog2(mm->chunk_size);
	order = fls(pages) - 1;
	if (order == 0)
		return -ENOSPC;

	root = &mm->free_tree[order];
	if (RB_EMPTY_ROOT(root))
		return -ENOSPC;

	iter = rb_last(root);
	while (iter) {
		block = rbtree_get_free_block(iter);

		/* Allocate blocks traversing RHS */
		rhs_offset = gpu_buddy_block_offset(block);
		err =  __gpu_buddy_alloc_range(mm, rhs_offset, size,
					       flags, &filled, blocks);
		if (!err || err != -ENOSPC)
			return err;

		lhs_size = max((size - filled), min_block_size);
		if (!IS_ALIGNED(lhs_size, min_block_size))
			lhs_size = round_up(lhs_size, min_block_size);

		/* Allocate blocks traversing LHS */
		lhs_offset = gpu_buddy_block_offset(block) - lhs_size;
		err =  __gpu_buddy_alloc_range(mm, lhs_offset, lhs_size,
					       flags, NULL, &blocks_lhs);
		if (!err) {
			list_splice(&blocks_lhs, blocks);
			return 0;
		} else if (err != -ENOSPC) {
			gpu_buddy_free_list_internal(mm, blocks);
			return err;
		}
		/* Free blocks for the next iteration */
		gpu_buddy_free_list_internal(mm, blocks);

		iter = rb_prev(iter);
	}

	return -ENOSPC;
}

/**
 * gpu_buddy_block_trim - free unused pages
 *
 * @mm: GPU buddy manager
 * @start: start address to begin the trimming.
 * @new_size: original size requested
 * @blocks: Input and output list of allocated blocks.
 * MUST contain single block as input to be trimmed.
 * On success will contain the newly allocated blocks
 * making up the @new_size. Blocks always appear in
 * ascending order
 *
 * For contiguous allocation, we round up the size to the nearest
 * power of two value, drivers consume *actual* size, so remaining
 * portions are unused and can be optionally freed with this function
 *
 * Returns:
 * 0 on success, error code on failure.
 */
int gpu_buddy_block_trim(struct gpu_buddy *mm,
			 u64 *start,
			 u64 new_size,
			 struct list_head *blocks)
{
	struct gpu_buddy_block *parent;
	struct gpu_buddy_block *block;
	u64 block_start, block_end;
	LIST_HEAD(dfs);
	bool was_clear;
	u64 new_start;
	int err;

	gpu_buddy_driver_lock_held(mm);

	if (!list_is_singular(blocks))
		return -EINVAL;

	block = list_first_entry(blocks,
				 struct gpu_buddy_block,
				 link);

	block_start = gpu_buddy_block_offset(block);
	block_end = block_start + gpu_buddy_block_size(mm, block);

	if (WARN_ON(!gpu_buddy_block_is_allocated(block)))
		return -EINVAL;

	if (new_size > gpu_buddy_block_size(mm, block))
		return -EINVAL;

	if (!new_size || !IS_ALIGNED(new_size, mm->chunk_size))
		return -EINVAL;

	if (new_size == gpu_buddy_block_size(mm, block))
		return 0;

	new_start = block_start;
	if (start) {
		new_start = *start;

		if (new_start < block_start)
			return -EINVAL;

		if (!IS_ALIGNED(new_start, mm->chunk_size))
			return -EINVAL;

		if (range_overflows(new_start, new_size, block_end))
			return -EINVAL;
	}

	list_del(&block->link);

	was_clear = gpu_buddy_block_is_clear(block);
	block->header &= ~GPU_BUDDY_HEADER_CLEAR;

	if (was_clear) {
		gpu_clear_tracker_mark_clear(&mm->clear,
					     gpu_buddy_block_offset(block),
					     gpu_buddy_block_size(mm, block));
		mm->clear_avail = mm->clear.total_clear;
	}

	mark_free(mm, block);
	mm->avail += gpu_buddy_block_size(mm, block);

	/* Prevent recursively freeing this node */
	parent = block->parent;
	block->parent = NULL;

	list_add(&block->tmp_link, &dfs);
	err =  __alloc_range(mm, &dfs, new_start, new_size,
			     was_clear ? GPU_BUDDY_CLEAR_ALLOCATION : 0,
			     blocks, NULL);
	if (err) {
		mark_allocated(mm, block);
		mm->avail -= gpu_buddy_block_size(mm, block);
		if (was_clear) {
			gpu_clear_tracker_mark_dirty(&mm->clear,
						     gpu_buddy_block_offset(block),
						     gpu_buddy_block_size(mm, block));
			mm->clear_avail = mm->clear.total_clear;
			block->header |= GPU_BUDDY_HEADER_CLEAR;
		}
		list_add(&block->link, blocks);
	}

	block->parent = parent;
	return err;
}
EXPORT_SYMBOL(gpu_buddy_block_trim);

static bool clear_steer_window(struct gpu_buddy *mm, u64 min_sz,
			       u64 *start, u64 *end, unsigned long *flags)
{
	struct gpu_clear_extent *ext =
		gpu_clear_tracker_find(&mm->clear, min_sz);

	if (!ext)
		return false;

	*start  = ext->start;
	*end    = ext->end;
	*flags |= GPU_BUDDY_RANGE_ALLOCATION;
	return true;
}

static struct gpu_buddy_block *
__gpu_buddy_alloc_blocks(struct gpu_buddy *mm,
			 u64 start, u64 end,
			 u64 size, u64 min_block_size,
			 unsigned int order,
			 unsigned long flags)
{
	struct gpu_buddy_block *block;
	bool steered = false;

	/* Steer cleared allocations to a cleared extent that fits the order */
	if (!(flags & GPU_BUDDY_RANGE_ALLOCATION) &&
	    (flags & GPU_BUDDY_CLEAR_ALLOCATION) && mm->clear_avail)
		steered = clear_steer_window(mm, mm->chunk_size << order,
					     &start, &end, &flags);

	if (flags & GPU_BUDDY_RANGE_ALLOCATION) {
		/* Allocate traversing within the range */
		block = __alloc_range_bias(mm, start, end, order, flags);
		if (!IS_ERR(block) || !steered)
			return block;

		flags &= ~GPU_BUDDY_RANGE_ALLOCATION;
	}

	if (size < min_block_size)
		/* Allocate from an offset-aligned region without size rounding */
		return gpu_buddy_offset_aligned_allocation(mm, size,
							   min_block_size,
							   flags);

	/* Allocate from freetree */
	return alloc_from_freetree(mm, order, flags);
}

/**
 * gpu_buddy_alloc_blocks - allocate power-of-two blocks
 *
 * @mm: GPU buddy manager to allocate from
 * @start: start of the allowed range for this block
 * @end: end of the allowed range for this block
 * @size: size of the allocation in bytes
 * @min_block_size: alignment of the allocation
 * @blocks: output list head to add allocated blocks
 * @flags: GPU_BUDDY_*_ALLOCATION flags
 *
 * alloc_range_bias() called on range limitations, which traverses
 * the tree and returns the desired block.
 *
 * alloc_from_freetree() called when *no* range restrictions
 * are enforced, which picks the block from the freetree.
 *
 * Returns:
 * 0 on success, error code on failure.
 */
int gpu_buddy_alloc_blocks(struct gpu_buddy *mm,
			   u64 start, u64 end, u64 size,
			   u64 min_block_size,
			   struct list_head *blocks,
			   unsigned long flags)
{
	struct gpu_buddy_block *block = NULL;
	u64 original_size, original_min_size;
	unsigned int min_order, order;
	LIST_HEAD(allocated);
	unsigned long pages;
	int err;

	gpu_buddy_driver_lock_held(mm);

	if (size < mm->chunk_size)
		return -EINVAL;

	if (min_block_size < mm->chunk_size)
		return -EINVAL;

	if (!is_power_of_2(min_block_size))
		return -EINVAL;

	if (!IS_ALIGNED(start | end | size, mm->chunk_size))
		return -EINVAL;

	if (end > mm->size)
		return -EINVAL;

	if (range_overflows(start, size, mm->size))
		return -EINVAL;

	/* Actual range allocation */
	if (start + size == end) {
		if (!IS_ALIGNED(start | end, min_block_size))
			return -EINVAL;

		return __gpu_buddy_alloc_range(mm, start, size, flags, NULL, blocks);
	}

	original_size = size;
	original_min_size = min_block_size;

	/* Roundup the size to power of 2 */
	if (flags & GPU_BUDDY_CONTIGUOUS_ALLOCATION) {
		size = roundup_pow_of_two(size);
		min_block_size = size;
		/*
		 * Normalize the requested size to min_block_size for regular allocations.
		 * Offset-aligned allocations intentionally skip size rounding.
		 */
	} else if (!gpu_buddy_can_offset_align(size, min_block_size)) {
		size = round_up(size, min_block_size);
	}

	pages = size >> ilog2(mm->chunk_size);
	order = fls(pages) - 1;
	min_order = ilog2(min_block_size) - ilog2(mm->chunk_size);

	if (order > mm->max_order || size > mm->size) {
		if ((flags & GPU_BUDDY_CONTIGUOUS_ALLOCATION) &&
		    !(flags & GPU_BUDDY_RANGE_ALLOCATION))
			return __alloc_contig_try_harder(mm, original_size,
							 original_min_size,
							 flags, blocks);

		return -EINVAL;
	}

	do {
		order = min(order, (unsigned int)fls(pages) - 1);
		BUG_ON(order > mm->max_order);
		/*
		 * Regular allocations must not allocate blocks smaller than min_block_size.
		 * Offset-aligned allocations deliberately bypass this constraint.
		 */
		BUG_ON(size >= min_block_size && order < min_order);

		do {
			block = __gpu_buddy_alloc_blocks(mm, start,
							 end,
							 size,
							 min_block_size,
							 order,
							 flags);
			if (!IS_ERR(block))
				break;

			if (size >= min_block_size && order > min_order) {
				order--;
				continue;
			}

			/*
			 * Try contiguous block allocation through
			 * try harder method.
			 */
			if (flags & GPU_BUDDY_CONTIGUOUS_ALLOCATION &&
			    !(flags & GPU_BUDDY_RANGE_ALLOCATION)) {
				err = __alloc_contig_try_harder(mm,
								original_size,
								original_min_size,
								flags,
								blocks);
				if (!err)
					return 0;
				if (err != -ENOSPC)
					return err;
				goto err_free;
			}
			err = -ENOSPC;
			goto err_free;
		} while (1);

		mark_allocated(mm, block);
		mm->avail -= gpu_buddy_block_size(mm, block);

		block->header &= ~GPU_BUDDY_HEADER_CLEAR;
		if (flags & GPU_BUDDY_CLEAR_ALLOCATION &&
		    gpu_clear_tracker_is_clear(&mm->clear,
					       gpu_buddy_block_offset(block),
					       gpu_buddy_block_size(mm, block)))
			block->header |= GPU_BUDDY_HEADER_CLEAR;

		gpu_clear_tracker_mark_dirty(&mm->clear,
					     gpu_buddy_block_offset(block),
					     gpu_buddy_block_size(mm, block));
		mm->clear_avail = mm->clear.total_clear;
		kmemleak_update_trace(block);
		list_add_tail(&block->link, &allocated);

		pages -= BIT(order);

		if (!pages)
			break;
	} while (1);

	/* Trim the allocated block to the required size */
	if (!(flags & GPU_BUDDY_TRIM_DISABLE) &&
	    original_size != size) {
		struct list_head *trim_list;
		LIST_HEAD(temp);
		u64 trim_size;

		trim_list = &allocated;
		trim_size = original_size;

		if (!list_is_singular(&allocated)) {
			block = list_last_entry(&allocated, typeof(*block), link);
			list_move(&block->link, &temp);
			trim_list = &temp;
			trim_size = gpu_buddy_block_size(mm, block) -
				(size - original_size);
		}

		gpu_buddy_block_trim(mm,
				     NULL,
				     trim_size,
				     trim_list);

		if (!list_empty(&temp))
			list_splice_tail(trim_list, &allocated);
	}

	list_splice_tail(&allocated, blocks);
	return 0;

err_free:
	gpu_buddy_free_list_internal(mm, &allocated);
	return err;
}
EXPORT_SYMBOL(gpu_buddy_alloc_blocks);

/**
 * gpu_buddy_block_print - print block information
 *
 * @mm: GPU buddy manager
 * @block: GPU buddy block
 */
void gpu_buddy_block_print(struct gpu_buddy *mm,
			   struct gpu_buddy_block *block)
{
	u64 start = gpu_buddy_block_offset(block);
	u64 size = gpu_buddy_block_size(mm, block);

	pr_info("%#018llx-%#018llx: %llu\n", start, start + size, size);
}
EXPORT_SYMBOL(gpu_buddy_block_print);

/**
 * gpu_buddy_print - print allocator state
 *
 * @mm: GPU buddy manager
 * @p: GPU printer to use
 */
void gpu_buddy_print(struct gpu_buddy *mm)
{
	int order;

	gpu_buddy_driver_lock_held(mm);
	pr_info("chunk_size: %lluKiB, total: %lluMiB, free: %lluMiB, clear_free: %lluMiB\n",
		mm->chunk_size >> 10, mm->size >> 20, mm->avail >> 20, mm->clear_avail >> 20);

	for (order = mm->max_order; order >= 0; order--) {
		struct gpu_buddy_block *block, *tmp;
		struct rb_root *root;
		u64 count = 0, clear = 0, free;

		root = &mm->free_tree[order];
		rbtree_postorder_for_each_entry_safe(block, tmp, root, rb) {
			BUG_ON(!gpu_buddy_block_is_free(block));
			count++;
			if (gpu_buddy_block_is_clear(block))
				clear++;
		}

		free = count * (mm->chunk_size << order);
		if (free < SZ_1M)
			pr_info("order-%2d free: %8llu KiB, blocks: %llu (clear: %llu)\n",
				order, free >> 10, count, clear);
		else
			pr_info("order-%2d free: %8llu MiB, blocks: %llu (clear: %llu)\n",
				order, free >> 20, count, clear);
	}
}
EXPORT_SYMBOL(gpu_buddy_print);

static void gpu_buddy_module_exit(void)
{
	kmem_cache_destroy(slab_extents);
	kmem_cache_destroy(slab_blocks);
}

static int __init gpu_buddy_module_init(void)
{
	slab_blocks = KMEM_CACHE(gpu_buddy_block, 0);
	if (!slab_blocks)
		return -ENOMEM;

	slab_extents = KMEM_CACHE(gpu_clear_extent, 0);
	if (!slab_extents) {
		kmem_cache_destroy(slab_blocks);
		return -ENOMEM;
	}

	return 0;
}

module_init(gpu_buddy_module_init);
module_exit(gpu_buddy_module_exit);

MODULE_DESCRIPTION("GPU Buddy Allocator");
MODULE_LICENSE("Dual MIT/GPL");
