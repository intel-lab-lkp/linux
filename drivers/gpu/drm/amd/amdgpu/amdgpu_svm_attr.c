// SPDX-License-Identifier: GPL-2.0 OR MIT
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
#include "amdgpu.h"

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/lockdep.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/slab.h>

static struct kmem_cache *amdgpu_svm_attr_range_cache;

struct attr_set_ctx {
	struct amdgpu_svm_attrs old_attrs;
	struct amdgpu_svm_attrs new_attrs;
	unsigned long start_page;
	unsigned long last_page;
};

struct attr_get_ctx {
	int32_t preferred_loc;
	int32_t prefetch_loc;
	enum amdgpu_ioctl_svm_access access;
	uint32_t granularity;
	uint32_t flags_and;
	bool has_range;
};

bool amdgpu_svm_attr_devmem_possible(struct amdgpu_svm *svm,
				     const struct amdgpu_svm_attrs *attrs)
{
	if (svm->adev->apu_prefer_gtt)
		return false;

	if (attrs->preferred_loc == AMDGPU_SVM_LOCATION_SYSMEM)
		return false;

	return true;
}

bool amdgpu_svm_attr_prefer_vram(struct amdgpu_svm *svm,
				 const struct amdgpu_svm_attrs *attrs)
{
	if (!amdgpu_svm_attr_devmem_possible(svm, attrs))
		return false;

	if (attrs->preferred_loc != AMDGPU_SVM_LOCATION_UNDEFINED &&
	    attrs->preferred_loc != AMDGPU_SVM_LOCATION_SYSMEM)
		return true;

	if (attrs->prefetch_loc != AMDGPU_SVM_LOCATION_UNDEFINED &&
	    attrs->prefetch_loc != AMDGPU_SVM_LOCATION_SYSMEM)
		return true;

	return false;
}

struct vm_area_struct *amdgpu_svm_check_vma(struct mm_struct *mm,
					unsigned long addr)
{
	const unsigned long flags = VM_IO | VM_PFNMAP | VM_MIXEDMAP;
	struct vm_area_struct *vma = vma_lookup(mm, addr);

	if (!vma)
		return ERR_PTR(-EFAULT);

	if (vma->vm_flags & flags)
		return ERR_PTR(-EOPNOTSUPP);

	return vma;
}

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

void amdgpu_svm_attr_set_default(struct amdgpu_svm *svm,
				 struct amdgpu_svm_attrs *attrs)
{
	attrs->preferred_loc = AMDGPU_SVM_LOCATION_UNDEFINED;
	attrs->prefetch_loc = AMDGPU_SVM_LOCATION_UNDEFINED;
	attrs->granularity = svm->default_granularity;
	attrs->flags = AMDGPU_SVM_ATTR_BIT_HOST_ACCESS | AMDGPU_SVM_ATTR_BIT_COHERENT;
	attrs->access = svm->xnack_enabled ?
		AMDGPU_SVM_ACCESS_ALLOW_MIGRATE : AMDGPU_SVM_ACCESS_INACCESSIBLE;
}

struct amdgpu_svm_attr_range *
amdgpu_svm_attr_find_locked(struct amdgpu_svm_attr_tree *attr_tree,
			   unsigned long page)
{
	struct interval_tree_node *node;

	node = interval_tree_iter_first(&attr_tree->tree, page, page);
	if (node)
		return container_of(node, struct amdgpu_svm_attr_range, it_node);

	return NULL;
}

struct amdgpu_svm_attr_range *
amdgpu_svm_attr_get_bounds_locked(struct amdgpu_svm_attr_tree *attr_tree,
				  unsigned long page,
				  unsigned long *start_page,
				  unsigned long *last_page)
{
	struct amdgpu_svm_attr_range *attr_range;
	struct interval_tree_node *node;
	struct rb_node *rb;

	attr_range = amdgpu_svm_attr_find_locked(attr_tree, page);
	if (attr_range) {
		*start_page = amdgpu_svm_attr_start_page(attr_range);
		*last_page = amdgpu_svm_attr_last_page(attr_range);
		return attr_range;
	}

	*start_page = 0;
	*last_page = ULONG_MAX;

	if (page == ULONG_MAX)
		return NULL;

	node = interval_tree_iter_first(&attr_tree->tree, page + 1, ULONG_MAX);
	if (node) {
		if (node->start > page)
			*last_page = node->start - 1;

		rb = rb_prev(&node->rb);
		if (rb) {
			node = container_of(rb, struct interval_tree_node, rb);
			if (node->last < page)
				*start_page = node->last + 1;
		}
	} else {
		rb = rb_last(&attr_tree->tree.rb_root);

		if (rb) {
			node = container_of(rb, struct interval_tree_node, rb);
			if (node->last < page)
				*start_page = node->last + 1;
		}
	}

	return NULL;
}

static bool attr_equal(const struct amdgpu_svm_attrs *a,
				 const struct amdgpu_svm_attrs *b)
{
	return a->flags == b->flags &&
	       a->preferred_loc == b->preferred_loc &&
	       a->prefetch_loc == b->prefetch_loc &&
		       a->granularity == b->granularity &&
		       a->access == b->access;
}

struct amdgpu_svm_attr_range *
amdgpu_svm_attr_range_alloc(unsigned long start_page,
			   unsigned long last_page,
			   const struct amdgpu_svm_attrs *attrs)
{
	struct amdgpu_svm_attr_range *range;

	range = kmem_cache_zalloc(amdgpu_svm_attr_range_cache, GFP_KERNEL);
	if (!range)
		return NULL;

	INIT_LIST_HEAD(&range->list);
	attr_set_interval(range, start_page, last_page);
	range->attrs = *attrs;
	return range;
}

void amdgpu_svm_attr_range_insert_locked(struct amdgpu_svm_attr_tree *attr_tree,
					 struct amdgpu_svm_attr_range *range)
{
	struct interval_tree_node *node;
	struct amdgpu_svm_attr_range *next;

	lockdep_assert_held(&attr_tree->lock);

	node = interval_tree_iter_first(&attr_tree->tree, amdgpu_svm_attr_start_page(range),
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
