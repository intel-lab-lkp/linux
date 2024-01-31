/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _LINUX_MAPLE_TREE_H
#define _LINUX_MAPLE_TREE_H
/*
 * Maple Tree - An RCU-safe adaptive tree for storing ranges
 * Copyright (c) 2018-2022 Oracle
 * Authors:     Liam R. Howlett <Liam.Howlett@Oracle.com>
 *              Matthew Wilcox <willy@infradead.org>
 */

#include <linux/maple_tree_types.h>

#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
/* #define CONFIG_MAPLE_RCU_DISABLED */

/**
 * DOC: Maple tree flags
 *
 * * MT_FLAGS_ALLOC_RANGE	- Track gaps in this tree
 * * MT_FLAGS_USE_RCU		- Operate in RCU mode
 * * MT_FLAGS_HEIGHT_OFFSET	- The position of the tree height in the flags
 * * MT_FLAGS_HEIGHT_MASK	- The mask for the maple tree height value
 * * MT_FLAGS_LOCK_MASK		- How the mt_lock is used
 * * MT_FLAGS_LOCK_IRQ		- Acquired irq-safe
 * * MT_FLAGS_LOCK_BH		- Acquired bh-safe
 * * MT_FLAGS_LOCK_EXTERN	- mt_lock is not used
 *
 * MAPLE_HEIGHT_MAX	The largest height that can be stored
 */
#define MT_FLAGS_ALLOC_RANGE	0x01
#define MT_FLAGS_USE_RCU	0x02
#define MT_FLAGS_HEIGHT_OFFSET	0x02
#define MT_FLAGS_HEIGHT_MASK	0x7C
#define MT_FLAGS_LOCK_MASK	0x300
#define MT_FLAGS_LOCK_IRQ	0x100
#define MT_FLAGS_LOCK_BH	0x200
#define MT_FLAGS_LOCK_EXTERN	0x300

#define MAPLE_HEIGHT_MAX	31


#define MAPLE_NODE_TYPE_MASK	0x0F
#define MAPLE_NODE_TYPE_SHIFT	0x03

#define MAPLE_RESERVED_RANGE	4096

#ifdef CONFIG_LOCKDEP
#define mt_lock_is_held(mt)                                             \
	(!(mt)->ma_external_lock || lock_is_held((mt)->ma_external_lock))

#define mt_write_lock_is_held(mt)					\
	(!(mt)->ma_external_lock ||					\
	 lock_is_held_type((mt)->ma_external_lock, 0))

#define mt_set_external_lock(mt, lock)					\
	(mt)->ma_external_lock = &(lock)->dep_map

#define mt_on_stack(mt)			(mt).ma_external_lock = NULL
#else
#define mt_lock_is_held(mt)		1
#define mt_write_lock_is_held(mt)	1
#define mt_set_external_lock(mt, lock)	do { } while (0)
#define mt_on_stack(mt)			do { } while (0)
#endif

/**
 * MTREE_INIT() - Initialize a maple tree
 * @name: The maple tree name
 * @__flags: The maple tree flags
 *
 */
#define MTREE_INIT(name, __flags) {					\
	.ma_lock = __SPIN_LOCK_UNLOCKED((name).ma_lock),		\
	.ma_flags = __flags,						\
	.ma_root = NULL,						\
}

/**
 * MTREE_INIT_EXT() - Initialize a maple tree with an external lock.
 * @name: The tree name
 * @__flags: The maple tree flags
 * @__lock: The external lock
 */
#ifdef CONFIG_LOCKDEP
#define MTREE_INIT_EXT(name, __flags, __lock) {				\
	.ma_external_lock = &(__lock).dep_map,				\
	.ma_flags = (__flags),						\
	.ma_root = NULL,						\
}
#else
#define MTREE_INIT_EXT(name, __flags, __lock)	MTREE_INIT(name, __flags)
#endif

#define DEFINE_MTREE(name)						\
	struct maple_tree name = MTREE_INIT(name, 0)

#define mtree_lock(mt)		spin_lock((&(mt)->ma_lock))
#define mtree_lock_nested(mas, subclass) \
		spin_lock_nested((&(mt)->ma_lock), subclass)
#define mtree_unlock(mt)	spin_unlock((&(mt)->ma_lock))

void *mtree_load(struct maple_tree *mt, unsigned long index);

int mtree_insert(struct maple_tree *mt, unsigned long index,
		void *entry, gfp_t gfp);
int mtree_insert_range(struct maple_tree *mt, unsigned long first,
		unsigned long last, void *entry, gfp_t gfp);
int mtree_alloc_range(struct maple_tree *mt, unsigned long *startp,
		void *entry, unsigned long size, unsigned long min,
		unsigned long max, gfp_t gfp);
int mtree_alloc_rrange(struct maple_tree *mt, unsigned long *startp,
		void *entry, unsigned long size, unsigned long min,
		unsigned long max, gfp_t gfp);

int mtree_store_range(struct maple_tree *mt, unsigned long first,
		      unsigned long last, void *entry, gfp_t gfp);
int mtree_store(struct maple_tree *mt, unsigned long index,
		void *entry, gfp_t gfp);
void *mtree_erase(struct maple_tree *mt, unsigned long index);

int mtree_dup(struct maple_tree *mt, struct maple_tree *new, gfp_t gfp);
int __mt_dup(struct maple_tree *mt, struct maple_tree *new, gfp_t gfp);

void mtree_destroy(struct maple_tree *mt);
void __mt_destroy(struct maple_tree *mt);

/**
 * mtree_empty() - Determine if a tree has any present entries.
 * @mt: Maple Tree.
 *
 * Context: Any context.
 * Return: %true if the tree contains only NULL pointers.
 */
static inline bool mtree_empty(const struct maple_tree *mt)
{
	return mt->ma_root == NULL;
}

/* Advanced API */

#define mas_lock(mas)           spin_lock(&((mas)->tree->ma_lock))
#define mas_lock_nested(mas, subclass) \
		spin_lock_nested(&((mas)->tree->ma_lock), subclass)
#define mas_unlock(mas)         spin_unlock(&((mas)->tree->ma_lock))

/*
 * Special values for ma_state.node.
 * MA_ERROR represents an errno.  After dropping the lock and attempting
 * to resolve the error, the walk would have to be restarted from the
 * top of the tree as the tree may have been modified.
 */
#define MA_ERROR(err) \
		((struct maple_enode *)(((unsigned long)err << 2) | 2UL))

#define MA_STATE(name, mt, first, end)					\
	struct ma_state name = {					\
		.tree = mt,						\
		.index = first,						\
		.last = end,						\
		.node = NULL,						\
		.status = ma_start,					\
		.min = 0,						\
		.max = ULONG_MAX,					\
		.alloc = NULL,						\
		.mas_flags = 0,						\
	}

#define MA_WR_STATE(name, ma_state, wr_entry)				\
	struct ma_wr_state name = {					\
		.mas = ma_state,					\
		.content = NULL,					\
		.entry = wr_entry,					\
	}

#define MA_TOPIARY(name, tree)						\
	struct ma_topiary name = {					\
		.head = NULL,						\
		.tail = NULL,						\
		.mtree = tree,						\
	}

void *mas_walk(struct ma_state *mas);
void *mas_store(struct ma_state *mas, void *entry);
void *mas_erase(struct ma_state *mas);
int mas_store_gfp(struct ma_state *mas, void *entry, gfp_t gfp);
void mas_store_prealloc(struct ma_state *mas, void *entry);
void *mas_find(struct ma_state *mas, unsigned long max);
void *mas_find_range(struct ma_state *mas, unsigned long max);
void *mas_find_rev(struct ma_state *mas, unsigned long min);
void *mas_find_range_rev(struct ma_state *mas, unsigned long max);
int mas_preallocate(struct ma_state *mas, void *entry, gfp_t gfp);

bool mas_nomem(struct ma_state *mas, gfp_t gfp);
void mas_pause(struct ma_state *mas);
void maple_tree_init(void);
void mas_destroy(struct ma_state *mas);
int mas_expected_entries(struct ma_state *mas, unsigned long nr_entries);

void *mas_prev(struct ma_state *mas, unsigned long min);
void *mas_prev_range(struct ma_state *mas, unsigned long max);
void *mas_next(struct ma_state *mas, unsigned long max);
void *mas_next_range(struct ma_state *mas, unsigned long max);

int mas_empty_area(struct ma_state *mas, unsigned long min, unsigned long max,
		   unsigned long size);
/*
 * This finds an empty area from the highest address to the lowest.
 * AKA "Topdown" version,
 */
int mas_empty_area_rev(struct ma_state *mas, unsigned long min,
		       unsigned long max, unsigned long size);

static inline bool mas_is_active(struct ma_state *mas)
{
	return mas->status == ma_active;
}

static inline bool mas_is_err(struct ma_state *mas)
{
	return mas->status == ma_error;
}

/**
 * mas_reset() - Reset a Maple Tree operation state.
 * @mas: Maple Tree operation state.
 *
 * Resets the error or walk state of the @mas so future walks of the
 * array will start from the root.  Use this if you have dropped the
 * lock and want to reuse the ma_state.
 *
 * Context: Any context.
 */
static __always_inline void mas_reset(struct ma_state *mas)
{
	mas->status = ma_start;
	mas->node = NULL;
}

/**
 * mas_for_each() - Iterate over a range of the maple tree.
 * @__mas: Maple Tree operation state (maple_state)
 * @__entry: Entry retrieved from the tree
 * @__max: maximum index to retrieve from the tree
 *
 * When returned, mas->index and mas->last will hold the entire range for the
 * entry.
 *
 * Note: may return the zero entry.
 */
#define mas_for_each(__mas, __entry, __max) \
	while (((__entry) = mas_find((__mas), (__max))) != NULL)

#ifdef CONFIG_DEBUG_MAPLE_TREE
enum mt_dump_format {
	mt_dump_dec,
	mt_dump_hex,
};

extern atomic_t maple_tree_tests_run;
extern atomic_t maple_tree_tests_passed;

void mt_dump(const struct maple_tree *mt, enum mt_dump_format format);
void mas_dump(const struct ma_state *mas);
void mas_wr_dump(const struct ma_wr_state *wr_mas);
void mt_validate(struct maple_tree *mt);
void mt_cache_shrink(void);
#define MT_BUG_ON(__tree, __x) do {					\
	atomic_inc(&maple_tree_tests_run);				\
	if (__x) {							\
		pr_info("BUG at %s:%d (%u)\n",				\
		__func__, __LINE__, __x);				\
		mt_dump(__tree, mt_dump_hex);				\
		pr_info("Pass: %u Run:%u\n",				\
			atomic_read(&maple_tree_tests_passed),		\
			atomic_read(&maple_tree_tests_run));		\
		dump_stack();						\
	} else {							\
		atomic_inc(&maple_tree_tests_passed);			\
	}								\
} while (0)

#define MAS_BUG_ON(__mas, __x) do {					\
	atomic_inc(&maple_tree_tests_run);				\
	if (__x) {							\
		pr_info("BUG at %s:%d (%u)\n",				\
		__func__, __LINE__, __x);				\
		mas_dump(__mas);					\
		mt_dump((__mas)->tree, mt_dump_hex);			\
		pr_info("Pass: %u Run:%u\n",				\
			atomic_read(&maple_tree_tests_passed),		\
			atomic_read(&maple_tree_tests_run));		\
		dump_stack();						\
	} else {							\
		atomic_inc(&maple_tree_tests_passed);			\
	}								\
} while (0)

#define MAS_WR_BUG_ON(__wrmas, __x) do {				\
	atomic_inc(&maple_tree_tests_run);				\
	if (__x) {							\
		pr_info("BUG at %s:%d (%u)\n",				\
		__func__, __LINE__, __x);				\
		mas_wr_dump(__wrmas);					\
		mas_dump((__wrmas)->mas);				\
		mt_dump((__wrmas)->mas->tree, mt_dump_hex);		\
		pr_info("Pass: %u Run:%u\n",				\
			atomic_read(&maple_tree_tests_passed),		\
			atomic_read(&maple_tree_tests_run));		\
		dump_stack();						\
	} else {							\
		atomic_inc(&maple_tree_tests_passed);			\
	}								\
} while (0)

#define MT_WARN_ON(__tree, __x)  ({					\
	int ret = !!(__x);						\
	atomic_inc(&maple_tree_tests_run);				\
	if (ret) {							\
		pr_info("WARN at %s:%d (%u)\n",				\
		__func__, __LINE__, __x);				\
		mt_dump(__tree, mt_dump_hex);				\
		pr_info("Pass: %u Run:%u\n",				\
			atomic_read(&maple_tree_tests_passed),		\
			atomic_read(&maple_tree_tests_run));		\
		dump_stack();						\
	} else {							\
		atomic_inc(&maple_tree_tests_passed);			\
	}								\
	unlikely(ret);							\
})

#define MAS_WARN_ON(__mas, __x) ({					\
	int ret = !!(__x);						\
	atomic_inc(&maple_tree_tests_run);				\
	if (ret) {							\
		pr_info("WARN at %s:%d (%u)\n",				\
		__func__, __LINE__, __x);				\
		mas_dump(__mas);					\
		mt_dump((__mas)->tree, mt_dump_hex);			\
		pr_info("Pass: %u Run:%u\n",				\
			atomic_read(&maple_tree_tests_passed),		\
			atomic_read(&maple_tree_tests_run));		\
		dump_stack();						\
	} else {							\
		atomic_inc(&maple_tree_tests_passed);			\
	}								\
	unlikely(ret);							\
})

#define MAS_WR_WARN_ON(__wrmas, __x) ({					\
	int ret = !!(__x);						\
	atomic_inc(&maple_tree_tests_run);				\
	if (ret) {							\
		pr_info("WARN at %s:%d (%u)\n",				\
		__func__, __LINE__, __x);				\
		mas_wr_dump(__wrmas);					\
		mas_dump((__wrmas)->mas);				\
		mt_dump((__wrmas)->mas->tree, mt_dump_hex);		\
		pr_info("Pass: %u Run:%u\n",				\
			atomic_read(&maple_tree_tests_passed),		\
			atomic_read(&maple_tree_tests_run));		\
		dump_stack();						\
	} else {							\
		atomic_inc(&maple_tree_tests_passed);			\
	}								\
	unlikely(ret);							\
})
#else
#define MT_BUG_ON(__tree, __x)		BUG_ON(__x)
#define MAS_BUG_ON(__mas, __x)		BUG_ON(__x)
#define MAS_WR_BUG_ON(__mas, __x)	BUG_ON(__x)
#define MT_WARN_ON(__tree, __x)		WARN_ON(__x)
#define MAS_WARN_ON(__mas, __x)		WARN_ON(__x)
#define MAS_WR_WARN_ON(__mas, __x)	WARN_ON(__x)
#endif /* CONFIG_DEBUG_MAPLE_TREE */

/**
 * __mas_set_range() - Set up Maple Tree operation state to a sub-range of the
 * current location.
 * @mas: Maple Tree operation state.
 * @start: New start of range in the Maple Tree.
 * @last: New end of range in the Maple Tree.
 *
 * set the internal maple state values to a sub-range.
 * Please use mas_set_range() if you do not know where you are in the tree.
 */
static inline void __mas_set_range(struct ma_state *mas, unsigned long start,
		unsigned long last)
{
	/* Ensure the range starts within the current slot */
	MAS_WARN_ON(mas, mas_is_active(mas) &&
		   (mas->index > start || mas->last < start));
	mas->index = start;
	mas->last = last;
}

/**
 * mas_set_range() - Set up Maple Tree operation state for a different index.
 * @mas: Maple Tree operation state.
 * @start: New start of range in the Maple Tree.
 * @last: New end of range in the Maple Tree.
 *
 * Move the operation state to refer to a different range.  This will
 * have the effect of starting a walk from the top; see mas_next()
 * to move to an adjacent index.
 */
static inline
void mas_set_range(struct ma_state *mas, unsigned long start, unsigned long last)
{
	mas_reset(mas);
	__mas_set_range(mas, start, last);
}

/**
 * mas_set() - Set up Maple Tree operation state for a different index.
 * @mas: Maple Tree operation state.
 * @index: New index into the Maple Tree.
 *
 * Move the operation state to refer to a different index.  This will
 * have the effect of starting a walk from the top; see mas_next()
 * to move to an adjacent index.
 */
static inline void mas_set(struct ma_state *mas, unsigned long index)
{

	mas_set_range(mas, index, index);
}

static inline bool mt_external_lock(const struct maple_tree *mt)
{
	return (mt->ma_flags & MT_FLAGS_LOCK_MASK) == MT_FLAGS_LOCK_EXTERN;
}

/**
 * mt_init_flags() - Initialise an empty maple tree with flags.
 * @mt: Maple Tree
 * @flags: maple tree flags.
 *
 * If you need to initialise a Maple Tree with special flags (eg, an
 * allocation tree), use this function.
 *
 * Context: Any context.
 */
static inline void mt_init_flags(struct maple_tree *mt, unsigned int flags)
{
	mt->ma_flags = flags;
	if (!mt_external_lock(mt))
		spin_lock_init(&mt->ma_lock);
	rcu_assign_pointer(mt->ma_root, NULL);
}

/**
 * mt_init() - Initialise an empty maple tree.
 * @mt: Maple Tree
 *
 * An empty Maple Tree.
 *
 * Context: Any context.
 */
static inline void mt_init(struct maple_tree *mt)
{
	mt_init_flags(mt, 0);
}

static inline bool mt_in_rcu(struct maple_tree *mt)
{
#ifdef CONFIG_MAPLE_RCU_DISABLED
	return false;
#endif
	return mt->ma_flags & MT_FLAGS_USE_RCU;
}

/**
 * mt_clear_in_rcu() - Switch the tree to non-RCU mode.
 * @mt: The Maple Tree
 */
static inline void mt_clear_in_rcu(struct maple_tree *mt)
{
	if (!mt_in_rcu(mt))
		return;

	if (mt_external_lock(mt)) {
		WARN_ON(!mt_lock_is_held(mt));
		mt->ma_flags &= ~MT_FLAGS_USE_RCU;
	} else {
		mtree_lock(mt);
		mt->ma_flags &= ~MT_FLAGS_USE_RCU;
		mtree_unlock(mt);
	}
}

/**
 * mt_set_in_rcu() - Switch the tree to RCU safe mode.
 * @mt: The Maple Tree
 */
static inline void mt_set_in_rcu(struct maple_tree *mt)
{
	if (mt_in_rcu(mt))
		return;

	if (mt_external_lock(mt)) {
		WARN_ON(!mt_lock_is_held(mt));
		mt->ma_flags |= MT_FLAGS_USE_RCU;
	} else {
		mtree_lock(mt);
		mt->ma_flags |= MT_FLAGS_USE_RCU;
		mtree_unlock(mt);
	}
}

static inline unsigned int mt_height(const struct maple_tree *mt)
{
	return (mt->ma_flags & MT_FLAGS_HEIGHT_MASK) >> MT_FLAGS_HEIGHT_OFFSET;
}

void *mt_find(struct maple_tree *mt, unsigned long *index, unsigned long max);
void *mt_find_after(struct maple_tree *mt, unsigned long *index,
		    unsigned long max);
void *mt_prev(struct maple_tree *mt, unsigned long index,  unsigned long min);
void *mt_next(struct maple_tree *mt, unsigned long index, unsigned long max);

/**
 * mt_for_each - Iterate over each entry starting at index until max.
 * @__tree: The Maple Tree
 * @__entry: The current entry
 * @__index: The index to start the search from. Subsequently used as iterator.
 * @__max: The maximum limit for @index
 *
 * This iterator skips all entries, which resolve to a NULL pointer,
 * e.g. entries which has been reserved with XA_ZERO_ENTRY.
 */
#define mt_for_each(__tree, __entry, __index, __max) \
	for (__entry = mt_find(__tree, &(__index), __max); \
		__entry; __entry = mt_find_after(__tree, &(__index), __max))

#endif /*_LINUX_MAPLE_TREE_H */
