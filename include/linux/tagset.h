/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tagsets
 *
 * Copyright (c) 2025, Oracle and/or its affiliates.
 * Author: Chuck Lever <chuck.lever@oracle.com>
 *
 * A tagset is a set of strings. See
 * Documentation/core-api/tagset.rst for how to use tagsets.
 *
 * Tagsets have no internal locking. Callers provide synchronization
 * between writers and readers, including a release/acquire (RCU, or
 * matching unlock/lock) publication boundary between tagset_finalize()
 * and the first concurrent query, and a grace period (or equivalent
 * reader drain) before tagset_destroy(). See the Thread Safety section
 * of Documentation/core-api/tagset.rst.
 */

#ifndef _LINUX_TAGSET_H
#define _LINUX_TAGSET_H

#include <linux/bug.h>
#include <linux/slab.h>

struct tagset {
	char		**ts_tags;
	unsigned int	ts_count;
	unsigned int	ts_capacity;
	bool		ts_finalized;
};

#define TAGSET_INIT \
	{ .ts_tags = NULL, .ts_count = 0, .ts_capacity = 0, .ts_finalized = false }
#define DEFINE_TAGSET(name) \
	struct tagset name = TAGSET_INIT

/**
 * tagset_for_each - Iterate over items in a tagset
 * @set: An initialized tagset containing zero or more items
 * @index: Index counter for the iteration
 * @tag: Tag retrieved from the tagset
 */
#define tagset_for_each(set, index, tag) \
	for ((index) = 0; (index) < (set)->ts_count && \
	     ((tag) = (set)->ts_tags[index], true); (index)++)

/**
 * tagset_init - Initialize an empty tagset
 * @set: tagset to be initialized
 */
static inline void tagset_init(struct tagset *set)
{
	set->ts_tags = NULL;
	set->ts_count = 0;
	set->ts_capacity = 0;
	set->ts_finalized = false;
}

/**
 * tagset_alloc - Pre-allocate space for tags
 * @set: An initialized tagset
 * @capacity: Number of tags to allocate space for
 * @gfp: Memory allocation flags
 *
 * This function may only be called once per tagset. Calling it on a
 * tagset that has already been allocated returns failure.
 *
 * @capacity may be zero. The tagset then represents an empty set:
 * tagset_add() rejects further additions, and tagset_finalize(),
 * tagset_is_member(), tagset_intersection(), and tagset_destroy()
 * all handle it correctly. The same state is produced by
 * DEFINE_TAGSET() and tagset_init() alone, so callers that know the
 * set is empty may skip tagset_alloc() entirely.
 *
 * Return:
 *   %true: allocation succeeded (or @capacity was zero)
 *   %false: allocation failed or tagset already allocated
 */
static inline __must_check bool
tagset_alloc(struct tagset *set, unsigned int capacity, gfp_t gfp)
{
	if (set->ts_tags)
		return false;
	if (capacity == 0)
		return true;

	set->ts_tags = kcalloc(capacity, sizeof(char *), gfp);
	if (!set->ts_tags)
		return false;
	set->ts_capacity = capacity;
	return true;
}

/**
 * tagset_is_empty - Determine if a tagset contains any tags
 * @set: An initialized tagset to be checked
 *
 * Return:
 *    %true: if @set is empty
 *    %false: if @set contains one or more tags
 */
static inline bool tagset_is_empty(const struct tagset *set)
{
	return set->ts_count == 0;
}

/**
 * tagset_count - Return the number of tags in a tagset
 * @set: An initialized tagset to be checked
 *
 * If called before tagset_finalize(), returns the number of tags
 * added. If called after, returns the number of unique tags.
 *
 * Return:
 *    The number of tags in @set
 */
static inline unsigned int tagset_count(const struct tagset *set)
{
	return set->ts_count;
}

/**
 * tagset_add - Add a tag to a tagset
 * @set: An initialized tagset with available capacity
 * @tag: non-NULL tag string to be added to @set, in kmalloc'd memory
 *
 * On success, @tag is now owned by @set and will be freed either
 * by tagset_finalize() (if a content duplicate) or tagset_destroy().
 * The tagset must have been allocated with sufficient capacity via
 * tagset_alloc(). Tags must not be added after tagset_finalize() has
 * been called. Callers must not hand the same pointer to tagset_add()
 * more than once: ownership has already transferred, and a second add
 * produces a double-free at tagset_destroy().
 *
 * Return:
 *   %true: @tag is now a member of @set
 *   %false: @tag could not be added (NULL or no capacity)
 */
static inline __must_check bool
tagset_add(struct tagset *set, char *tag)
{
	if (WARN_ON_ONCE(set->ts_finalized))
		return false;
	if (!tag || set->ts_count >= set->ts_capacity)
		return false;
	set->ts_tags[set->ts_count++] = tag;
	return true;
}

/**
 * tagset_add_dup - Add a copy of a tag to a tagset
 * @set: An initialized tagset with available capacity
 * @tag: non-NULL tag string to be copied and added to @set
 * @gfp: Memory allocation flags
 *
 * On success, @tag will have been copied into a kmalloc'd
 * buffer. The caller can release @tag immediately. Tags must not
 * be added after tagset_finalize() has been called.
 *
 * Return:
 *   %true: @tag is now a member of @set
 *   %false: @tag could not be added (NULL, no capacity, or ENOMEM)
 */
static inline __must_check bool
tagset_add_dup(struct tagset *set, const char *tag, gfp_t gfp)
{
	char *entry;

	if (WARN_ON_ONCE(set->ts_finalized))
		return false;
	if (!tag || set->ts_count >= set->ts_capacity)
		return false;
	entry = kstrdup(tag, gfp);
	if (!entry)
		return false;
	set->ts_tags[set->ts_count++] = entry;
	return true;
}

/* Implemented in lib/tagset.c */
void tagset_finalize(struct tagset *set);
void tagset_destroy(struct tagset *set);
bool tagset_is_member(const struct tagset *set, const char *tag);
bool tagset_copy(struct tagset *dest, const struct tagset *src, gfp_t gfp);
bool tagset_intersection(const struct tagset *set1, const struct tagset *set2);

#endif /* _LINUX_TAGSET_H */
