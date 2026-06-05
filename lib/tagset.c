// SPDX-License-Identifier: GPL-2.0
/*
 * Tagsets - a sorted set of strings
 *
 * Copyright (c) 2025, Oracle and/or its affiliates.
 * Author: Chuck Lever <chuck.lever@oracle.com>
 */

#include <linux/bug.h>
#include <linux/export.h>
#include <linux/sort.h>
#include <linux/tagset.h>

static int tagset_cmp(const void *a, const void *b)
{
	const char * const *sa = a;
	const char * const *sb = b;

	return strcmp(*sa, *sb);
}

/**
 * tagset_finalize - Sort the tagset and remove duplicates
 * @set: An initialized tagset that has been populated
 *
 * This must be called after all tags have been added and before
 * calling tagset_is_member() or tagset_intersection(). Duplicate
 * tags are removed and their memory is freed.
 */
void tagset_finalize(struct tagset *set)
{
	unsigned int i, j;

	if (WARN_ON_ONCE(set->ts_finalized))
		return;
	if (set->ts_count > 1) {
		sort(set->ts_tags, set->ts_count, sizeof(char *),
		     tagset_cmp, NULL);

		/* Remove duplicates in place */
		for (i = 0, j = 1; j < set->ts_count; j++) {
			if (strcmp(set->ts_tags[i], set->ts_tags[j]) == 0)
				kfree(set->ts_tags[j]);
			else
				set->ts_tags[++i] = set->ts_tags[j];
		}
		set->ts_count = i + 1;
	}
	set->ts_finalized = true;
}
EXPORT_SYMBOL_GPL(tagset_finalize);

/**
 * tagset_destroy - Release tagset resources
 * @set: tagset to be destroyed
 */
void tagset_destroy(struct tagset *set)
{
	unsigned int i;

	for (i = 0; i < set->ts_count; i++)
		kfree(set->ts_tags[i]);
	kfree(set->ts_tags);
	tagset_init(set);
}
EXPORT_SYMBOL_GPL(tagset_destroy);

/**
 * tagset_is_member - Check if a tag is already a member of a tagset
 * @set: An initialized and finalized tagset to be checked
 * @tag: tag string to search for
 *
 * Uses binary search. The tagset must have been finalized first.
 *
 * Return:
 *   %true: if @tag is a member of @set
 *   %false: if @tag is not a member of @set
 */
bool tagset_is_member(const struct tagset *set, const char *tag)
{
	unsigned int low = 0, high = set->ts_count;

	WARN_ON_ONCE(!set->ts_finalized);
	while (low < high) {
		unsigned int mid = low + (high - low) / 2;
		int cmp = strcmp(tag, set->ts_tags[mid]);

		if (cmp == 0)
			return true;
		if (cmp < 0)
			high = mid;
		else
			low = mid + 1;
	}
	return false;
}
EXPORT_SYMBOL_GPL(tagset_is_member);

/**
 * tagset_copy - Duplicate tags to another tagset
 * @dest: An empty, initialized tagset to be filled
 * @src: An initialized tagset to be copied from
 * @gfp: Memory allocation flags
 *
 * @dest must be initialized -- via TAGSET_INIT, DEFINE_TAGSET,
 * tagset_init(), or a prior tagset_destroy() -- and must hold no
 * tags. Passing a populated tagset leaks its contents.
 *
 * On success @dest is populated and ready for use; no call to
 * tagset_finalize() is required. On failure @dest is left as an
 * empty, finalized tagset, so callers can safely run queries
 * against it without first checking the return value.
 *
 * Return:
 *   %true: All tags in @src were copied to @dest
 *   %false: A failure occurred; @dest is empty and finalized
 */
bool tagset_copy(struct tagset *dest, const struct tagset *src, gfp_t gfp)
{
	unsigned int i;

	/* src is already sorted, so dest is too */
	dest->ts_finalized = src->ts_finalized;
	if (src->ts_count == 0)
		return true;
	if (!tagset_alloc(dest, src->ts_count, gfp))
		goto out_fail;
	for (i = 0; i < src->ts_count; i++) {
		char *entry = kstrdup(src->ts_tags[i], gfp);

		if (!entry)
			goto out_fail;
		dest->ts_tags[dest->ts_count++] = entry;
	}
	return true;

out_fail:
	tagset_destroy(dest);
	dest->ts_finalized = true;
	return false;
}
EXPORT_SYMBOL_GPL(tagset_copy);

/**
 * tagset_intersection - Report if there are common tags
 * @set1: An initialized and finalized tagset
 * @set2: Another initialized and finalized tagset
 *
 * Uses merge-style comparison of two sorted arrays for O(N+M)
 * complexity.
 *
 * Return:
 *   %true: @set1 and @set2 have at least one common tag
 *   %false: @set1 and @set2 have no tags in common
 */
bool tagset_intersection(const struct tagset *set1, const struct tagset *set2)
{
	unsigned int i = 0, j = 0;

	WARN_ON_ONCE(!set1->ts_finalized);
	WARN_ON_ONCE(!set2->ts_finalized);
	while (i < set1->ts_count && j < set2->ts_count) {
		int cmp = strcmp(set1->ts_tags[i], set2->ts_tags[j]);

		if (cmp == 0)
			return true;
		if (cmp < 0)
			i++;
		else
			j++;
	}
	return false;
}
EXPORT_SYMBOL_GPL(tagset_intersection);
