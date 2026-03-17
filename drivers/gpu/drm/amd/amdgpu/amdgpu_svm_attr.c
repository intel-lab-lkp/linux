/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * Copyright 2026 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "amdgpu_svm.h"
#include "amdgpu_svm_attr.h"
#include "amdgpu_svm_range.h"

#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/lockdep.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/slab.h>

static struct kmem_cache *amdgpu_svm_attr_range_cache;

struct attr_get_ctx {
	int32_t preferred_loc;
	int32_t prefetch_loc;
	enum amdgpu_svm_attr_access access;
	uint32_t granularity;
	uint32_t flags_and;
	uint32_t flags_or;
	bool has_range;
};

int amdgpu_svm_attr_cache_init(void)
{
	amdgpu_svm_attr_range_cache = AMDGPU_SVM_KMEM_CACHE_CREATE(
				"amdgpu_svm_attr_range_cache", struct amdgpu_svm_attr_range);
	if (!amdgpu_svm_attr_range_cache)
		return -ENOMEM;

	return 0;
}

void amdgpu_svm_attr_cache_fini(void)
{
	AMDGPU_SVM_KMEM_CACHE_DESTROY(amdgpu_svm_attr_range_cache);
}

static void attr_set_interval(struct amdgpu_svm_attr_range *range,
				unsigned long start_page,
				unsigned long last_page)
{
	range->it_node.start = start_page;
	range->it_node.last = last_page;
}

static unsigned long attr_start_page(const struct amdgpu_svm_attr_range *range)
{
	return range->it_node.start;
}

static unsigned long attr_last_page(const struct amdgpu_svm_attr_range *range)
{
	return range->it_node.last;
}

static void attr_set_default(struct amdgpu_svm *svm,
			     struct amdgpu_svm_attrs *attrs)
{
	attrs->preferred_loc = AMDGPU_SVM_LOCATION_UNDEFINED;
	attrs->prefetch_loc = AMDGPU_SVM_LOCATION_UNDEFINED;
	attrs->granularity = svm->default_granularity;
	attrs->flags = AMDGPU_SVM_FLAG_HOST_ACCESS | AMDGPU_SVM_FLAG_COHERENT;
	attrs->access = svm->xnack_enabled ?
		AMDGPU_SVM_ACCESS_ENABLE : AMDGPU_SVM_ACCESS_NONE;
}

void amdgpu_svm_attr_lookup_page_locked(struct amdgpu_svm_attr_tree *attr_tree,
					  unsigned long page,
					  struct amdgpu_svm_attrs *attrs,
					  unsigned long *range_last)
{
	struct interval_tree_node *node;
	struct amdgpu_svm_attr_range *range;

	node = interval_tree_iter_first(&attr_tree->tree, page, page);
	if (node) {
		range = container_of(node, struct amdgpu_svm_attr_range, it_node);
		*attrs = range->attrs;
		*range_last = range->it_node.last;
		return;
	}

	attr_set_default(attr_tree->svm, attrs);
	*range_last = ULONG_MAX;

	if (page == ULONG_MAX)
		return;

	node = interval_tree_iter_first(&attr_tree->tree, page + 1, ULONG_MAX);
	if (!node)
		return;

	range = container_of(node, struct amdgpu_svm_attr_range, it_node);
	if (range->it_node.start > page)
		*range_last = range->it_node.start - 1;
}

static bool amdgpu_svm_attr_equal(const struct amdgpu_svm_attrs *a,
				 const struct amdgpu_svm_attrs *b)
{
	return a->flags == b->flags &&
	       a->preferred_loc == b->preferred_loc &&
	       a->prefetch_loc == b->prefetch_loc &&
		       a->granularity == b->granularity &&
		       a->access == b->access;
}

static struct amdgpu_svm_attr_range *
attr_alloc_range(unsigned long start,
			   unsigned long last,
		   const struct amdgpu_svm_attrs *attrs)
{
	struct amdgpu_svm_attr_range *range;

	range = kmem_cache_zalloc(amdgpu_svm_attr_range_cache, GFP_KERNEL);
	if (!range)
		return NULL;

	INIT_LIST_HEAD(&range->list);
	attr_set_interval(range, start, last);
	range->attrs = *attrs;
	return range;
}

static void attr_insert_range_locked(struct amdgpu_svm_attr_tree *attr_tree,
					  struct amdgpu_svm_attr_range *range)
{
	struct interval_tree_node *node;
	struct amdgpu_svm_attr_range *next;

	lockdep_assert_held(&attr_tree->lock);

	node = interval_tree_iter_first(&attr_tree->tree, attr_start_page(range),
					ULONG_MAX);
	if (node) {
		next = container_of(node, struct amdgpu_svm_attr_range, it_node);
		list_add_tail(&range->list, &next->list);
	} else {
		list_add_tail(&range->list, &attr_tree->range_list);
	}

	interval_tree_insert(&range->it_node, &attr_tree->tree);
}

static void attr_remove_range_locked(struct amdgpu_svm_attr_tree *attr_tree,
					  struct amdgpu_svm_attr_range *range,
					  bool free_range)
{
	lockdep_assert_held(&attr_tree->lock);

	interval_tree_remove(&range->it_node, &attr_tree->tree);
	list_del_init(&range->list);
	if (free_range)
		kmem_cache_free(amdgpu_svm_attr_range_cache, range);
}

struct amdgpu_svm_attr_tree *
amdgpu_svm_attr_tree_create(struct amdgpu_svm *svm)
{
	struct amdgpu_svm_attr_tree *attr_tree;

	attr_tree = kzalloc(sizeof(*attr_tree), GFP_KERNEL);
	if (!attr_tree)
		return NULL;

	mutex_init(&attr_tree->lock);
	attr_tree->tree = RB_ROOT_CACHED;
	INIT_LIST_HEAD(&attr_tree->range_list);
	attr_tree->svm = svm;
	return attr_tree;
}

void amdgpu_svm_attr_tree_destroy(struct amdgpu_svm_attr_tree *attr_tree)
{
	struct amdgpu_svm_attr_range *range, *tmp;

	if (!attr_tree)
		return;

	mutex_lock(&attr_tree->lock);
	list_for_each_entry_safe(range, tmp, &attr_tree->range_list, list) {
		interval_tree_remove(&range->it_node, &attr_tree->tree);
		list_del_init(&range->list);
		kmem_cache_free(amdgpu_svm_attr_range_cache, range);
	}
	mutex_unlock(&attr_tree->lock);

	mutex_destroy(&attr_tree->lock);
	kfree(attr_tree);
}

static void attr_get_ctx_add(struct attr_get_ctx *ctx,
			       const struct amdgpu_svm_attrs *attrs)
{
	if (!ctx->has_range) {
		ctx->preferred_loc = attrs->preferred_loc;
		ctx->prefetch_loc = attrs->prefetch_loc;
		ctx->granularity = attrs->granularity;
		ctx->access = attrs->access;
		ctx->flags_and = attrs->flags;
		ctx->flags_or = attrs->flags;
		ctx->has_range = true;
		return;
	}

	if (ctx->preferred_loc != attrs->preferred_loc)
		ctx->preferred_loc = AMDGPU_SVM_LOCATION_UNDEFINED;
	if (ctx->prefetch_loc != attrs->prefetch_loc)
		ctx->prefetch_loc = AMDGPU_SVM_LOCATION_UNDEFINED;
	if (attrs->granularity < ctx->granularity)
		ctx->granularity = attrs->granularity;
	if (ctx->access != attrs->access)
		ctx->access = AMDGPU_SVM_ACCESS_NONE;
	ctx->flags_and &= attrs->flags;
	ctx->flags_or |= attrs->flags;
}

static int attr_get_ctx_to_result(const struct attr_get_ctx *ctx,
				uint32_t nattr,
				struct drm_amdgpu_svm_attribute *attrs)
{
	uint32_t i;

	for (i = 0; i < nattr; i++) {
		switch (attrs[i].type) {
		case AMDGPU_SVM_ATTR_PREFERRED_LOC:
			attrs[i].value = ctx->preferred_loc;
			break;
		case AMDGPU_SVM_ATTR_PREFETCH_LOC:
			attrs[i].value = ctx->prefetch_loc;
			break;
		case AMDGPU_SVM_ATTR_ACCESS:
			if (ctx->access == AMDGPU_SVM_ACCESS_ENABLE)
				attrs[i].type = AMDGPU_SVM_ATTR_ACCESS;
			else if (ctx->access == AMDGPU_SVM_ACCESS_IN_PLACE)
				attrs[i].type = AMDGPU_SVM_ATTR_ACCESS_IN_PLACE;
			else
				attrs[i].type = AMDGPU_SVM_ATTR_NO_ACCESS;
			break;
		case AMDGPU_SVM_ATTR_SET_FLAGS:
			attrs[i].value = ctx->flags_and;
			break;
		case AMDGPU_SVM_ATTR_CLR_FLAGS:
			attrs[i].value = ~ctx->flags_or;
			break;
		case AMDGPU_SVM_ATTR_GRANULARITY:
			attrs[i].value = ctx->granularity;
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

int amdgpu_svm_attr_get(struct amdgpu_svm_attr_tree *attr_tree,
			uint64_t start, uint64_t size,
			uint32_t nattr,
			struct drm_amdgpu_svm_attribute *attrs)
{
	struct amdgpu_svm_attrs default_attrs;
	struct attr_get_ctx ctx = { 0 };
	struct interval_tree_node *node;
	unsigned long start_page, last_page, cursor;
	int r;

	start_page = start >> PAGE_SHIFT;
	last_page = (start + size - 1) >> PAGE_SHIFT;

	mutex_lock(&attr_tree->lock);
	attr_set_default(attr_tree->svm, &default_attrs);
	node = interval_tree_iter_first(&attr_tree->tree, start_page, last_page);

	cursor = start_page;
	while (cursor <= last_page) {
		const struct amdgpu_svm_attrs *range_attrs;
		unsigned long range_last = last_page;
		struct amdgpu_svm_attr_range *range = NULL;
		unsigned long next;

		if (node) {
			range = container_of(node, struct amdgpu_svm_attr_range,
					     it_node);

			if (attr_last_page(range) < cursor) {
				node = interval_tree_iter_next(node, start_page,
							      last_page);
				continue;
			}

			if (attr_start_page(range) <= cursor) {
				range_last = min(last_page, attr_last_page(range));
				node = interval_tree_iter_next(node, start_page,
							      last_page);
			} else {
				range_last = min(last_page,
						 attr_start_page(range) - 1);
				range = NULL;
			}
		}

		range_attrs = range ? &range->attrs : &default_attrs;
		attr_get_ctx_add(&ctx, range_attrs);

		if (range_last == ULONG_MAX)
			break;

		next = range_last + 1;
		if (next <= cursor)
			break;
		cursor = next;
	}

	if (!ctx.has_range)
		attr_get_ctx_add(&ctx, &default_attrs);

	r = attr_get_ctx_to_result(&ctx, nattr, attrs);
	mutex_unlock(&attr_tree->lock);
	return r;
}
