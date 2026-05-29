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

static void attr_change_ctx_set(
		struct attr_set_ctx *change,
		const struct amdgpu_svm_attrs *old_attrs,
		const struct amdgpu_svm_attrs *new_attrs,
		unsigned long start_page,
		unsigned long last_page)
{
	change->old_attrs = *old_attrs;
	change->new_attrs = *new_attrs;
	change->start_page = start_page;
	change->last_page = last_page;
}

static inline int attr_check_preferred_loc(uint32_t value)
{
	/* cause one svm one gpu so value > 0 then means preferred loc is this GPU */
	if (value == AMDGPU_SVM_LOCATION_SYSMEM || value == AMDGPU_SVM_LOCATION_UNDEFINED)
		return 0;

	return 0;
}

static inline int attr_check_prefetch_loc(uint32_t value)
{
	/* cause one svm one gpu so value > 0 then means prefetch loc is this GPU
	 * keep prefetch loc to adapt to KFD API
	 */
	if (value == AMDGPU_SVM_LOCATION_SYSMEM)
		return 0;

	if (value == AMDGPU_SVM_LOCATION_UNDEFINED)
		return -EINVAL;

	return 0;
}

static inline int attr_check_access(uint32_t value)
{
	switch (value) {
	case AMDGPU_SVM_ACCESS_INACCESSIBLE:
	case AMDGPU_SVM_ACCESS_IN_PLACE:
	case AMDGPU_SVM_ACCESS_ALLOW_MIGRATE:
		return 0;
	default:
		return -EINVAL;
	}
}

static bool attr_flag_type_to_bit(uint32_t type, uint32_t *flag_bit)
{
	if (type < AMDGPU_SVM_ATTR_HOST_ACCESS ||
	    type > AMDGPU_SVM_ATTR_GPU_READ_MOSTLY)
		return false;

	*flag_bit = 1u << (type - AMDGPU_SVM_ATTR_HOST_ACCESS);
	return true;
}

static inline int attr_check_flag_value(uint32_t value)
{
	if (value > 1)
		return -EINVAL;

	return 0;
}

static inline int attr_check_flag_attr(uint32_t type, uint32_t value)
{
	uint32_t flag_bit;
	int ret;

	if (!attr_flag_type_to_bit(type, &flag_bit))
		return -EINVAL;

	ret = attr_check_flag_value(value);
	if (ret)
		return ret;

	return 0;
}

static inline int attr_check_granularity(uint32_t value)
{
	return 0;
}

int
amdgpu_svm_attr_check_vm_bo(struct amdgpu_svm_attr_tree *attr_tree,
			    unsigned long start_page,
			    unsigned long last_page,
			    unsigned long *bo_start, unsigned long *bo_last)
{
	struct amdgpu_svm *svm = attr_tree->svm;
	struct amdgpu_vm *vm = svm->vm;
	struct interval_tree_node *node;
	int r;

	r = amdgpu_bo_reserve(vm->root.bo, false);
	if (r)
		return r;

	node = interval_tree_iter_first(&vm->va, start_page, last_page);
	if (node) {
		AMDGPU_SVM_ERR("SVM range [0x%lx 0x%lx] overlaps with BO mapping [0x%lx 0x%lx]\n",
			       start_page, last_page, node->start, node->last);
		if (bo_start)
			*bo_start = node->start;
		if (bo_last)
			*bo_last = node->last;
		amdgpu_bo_unreserve(vm->root.bo);
		return -EADDRINUSE;
	}

	amdgpu_bo_unreserve(vm->root.bo);
	return 0;
}

static int
amdgpu_svm_attr_validate_range_vma(struct amdgpu_svm_attr_tree *attr_tree,
				   unsigned long start_page,
				   unsigned long last_page)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	unsigned long start, end;
	int ret = 0;

	if (start_page > last_page)
		return -EINVAL;

	if (last_page == ULONG_MAX)
		return -EINVAL;

	start = start_page << PAGE_SHIFT;
	end = (last_page + 1) << PAGE_SHIFT;
	mm = attr_tree->svm->gpusvm.mm;
	if (!mm)
		return -EFAULT;

	mmap_read_lock(mm);
	while (start < end) {
		vma = amdgpu_svm_check_vma(mm, start);
		if (IS_ERR(vma)) {
			ret = PTR_ERR(vma);
			break;
		}

		start = min(end, vma->vm_end);
	}
	mmap_read_unlock(mm);

	return ret;
}

static int attr_set_validate(const struct drm_amdgpu_svm_attribute *attr)
{
	switch (attr->type) {
	case AMDGPU_SVM_ATTR_PREFERRED_LOC:
		return attr_check_preferred_loc(attr->value);
	case AMDGPU_SVM_ATTR_PREFETCH_LOC:
		return attr_check_prefetch_loc(attr->value);
	case AMDGPU_SVM_ATTR_ACCESS:
		return attr_check_access(attr->value);
	case AMDGPU_SVM_ATTR_GRANULARITY:
		return attr_check_granularity(attr->value);
	case AMDGPU_SVM_ATTR_HOST_ACCESS:
	case AMDGPU_SVM_ATTR_COHERENT:
	case AMDGPU_SVM_ATTR_HIVE_LOCAL:
	case AMDGPU_SVM_ATTR_GPU_RO:
	case AMDGPU_SVM_ATTR_GPU_EXEC:
	case AMDGPU_SVM_ATTR_GPU_READ_MOSTLY:
	case AMDGPU_SVM_ATTR_EXT_COHERENT:
		return attr_check_flag_attr(attr->type, attr->value);
	default:
		return -EINVAL;
	}
}

static void attr_apply_flag(struct amdgpu_svm_attrs *attrs,
			    uint32_t type, uint32_t value)
{
	uint32_t flag_bit;

	if (!attr_flag_type_to_bit(type, &flag_bit))
		return;

	if (value)
		attrs->flags |= flag_bit;
	else
		attrs->flags &= ~flag_bit;
}

static void attr_apply(struct amdgpu_svm_attrs *attrs,
					uint32_t nattr,
					const struct drm_amdgpu_svm_attribute *pattrs)
{
	const struct drm_amdgpu_svm_attribute *attr;

	for (attr = pattrs; nattr--; attr++) {
		switch (attr->type) {
		case AMDGPU_SVM_ATTR_PREFERRED_LOC:
			attrs->preferred_loc = (int32_t)attr->value;
			break;
		case AMDGPU_SVM_ATTR_PREFETCH_LOC:
			attrs->prefetch_loc = (int32_t)attr->value;
			break;
		case AMDGPU_SVM_ATTR_ACCESS:
			attrs->access = (enum amdgpu_ioctl_svm_access)attr->value;
			break;
		case AMDGPU_SVM_ATTR_HOST_ACCESS:
		case AMDGPU_SVM_ATTR_COHERENT:
		case AMDGPU_SVM_ATTR_HIVE_LOCAL:
		case AMDGPU_SVM_ATTR_GPU_RO:
		case AMDGPU_SVM_ATTR_GPU_EXEC:
		case AMDGPU_SVM_ATTR_GPU_READ_MOSTLY:
		case AMDGPU_SVM_ATTR_EXT_COHERENT:
			attr_apply_flag(attrs, attr->type, attr->value);
			break;
		case AMDGPU_SVM_ATTR_GRANULARITY:
			attrs->granularity = min_t(uint32_t, attr->value, 0x3f);
			break;
		default:
			break;
		}
	}
}

static bool attr_same_attrs(const struct amdgpu_svm_attr_range *range,
			    uint32_t nattr,
			    const struct drm_amdgpu_svm_attribute *attrs)
{
	struct amdgpu_svm_attrs target;

	target = range->attrs;
	attr_apply(&target, nattr, attrs);
	return attr_equal(&range->attrs, &target);
}

static int
amdgpu_svm_attr_set_hole(struct amdgpu_svm_attr_tree *attr_tree,
			  const struct amdgpu_svm_attrs *default_attrs,
			  unsigned long start_page, unsigned long last_page,
			  uint32_t nattr,
			  const struct drm_amdgpu_svm_attribute *attrs,
			  struct attr_set_ctx *change)
{
	struct amdgpu_svm_attrs new_attrs;
	struct amdgpu_svm_attr_range *range;

	lockdep_assert_held(&attr_tree->lock);

	if (start_page > last_page)
		return 0;

	new_attrs = *default_attrs;
	attr_apply(&new_attrs, nattr, attrs);

	/* Always create a range entry even when attrs equal defaults */
	range = amdgpu_svm_attr_range_alloc(start_page, last_page, &new_attrs);
	if (!range)
		return -ENOMEM;

	amdgpu_svm_attr_range_insert_locked(attr_tree, range);

	attr_change_ctx_set(change, default_attrs,
				       &new_attrs, start_page, last_page);
	return 0;
}

static int
amdgpu_svm_attr_set_existing(struct amdgpu_svm_attr_tree *attr_tree,
			     struct amdgpu_svm_attr_range *range,
			     unsigned long start_page, unsigned long last_page,
			     uint32_t nattr,
			     const struct drm_amdgpu_svm_attribute *attrs,
			     struct attr_set_ctx *change)
{
	unsigned long range_start = amdgpu_svm_attr_start_page(range);
	unsigned long range_last = amdgpu_svm_attr_last_page(range);
	struct amdgpu_svm_attr_range *left = NULL;
	struct amdgpu_svm_attr_range *right = NULL;
	struct amdgpu_svm_attrs old_attrs;
	struct amdgpu_svm_attrs new_attrs;

	lockdep_assert_held(&attr_tree->lock);

	old_attrs = range->attrs;

	if (attr_same_attrs(range, nattr, attrs)) {
		/* Report old==new so apply_attr_change can decide */
		attr_change_ctx_set(change, &old_attrs,
					       &old_attrs,
					       start_page, last_page);
		return 0;
	}

	new_attrs = old_attrs;
	attr_apply(&new_attrs, nattr, attrs);

	/* only need to update attr */
	if (start_page == range_start && last_page == range_last) {
		range->attrs = new_attrs;
		attr_change_ctx_set(change, &old_attrs,
					       &new_attrs, start_page, last_page);
		return 0;
	}

	/* split head */
	if (start_page > range_start) {
		left = amdgpu_svm_attr_range_alloc(range_start, start_page - 1, &old_attrs);
		if (!left)
			return -ENOMEM;
	}

	/* split tail */
	if (last_page < range_last) {
		right = amdgpu_svm_attr_range_alloc(last_page + 1, range_last, &old_attrs);
		if (!right) {
			if (left)
				kmem_cache_free(amdgpu_svm_attr_range_cache, left);
			return -ENOMEM;
		}
	}

	attr_remove_range_locked(attr_tree, range, false);
	if (left)
		amdgpu_svm_attr_range_insert_locked(attr_tree, left);
	attr_set_interval(range, start_page, last_page);
	range->attrs = new_attrs;
	amdgpu_svm_attr_range_insert_locked(attr_tree, range);
	if (right)
		amdgpu_svm_attr_range_insert_locked(attr_tree, right);

	attr_change_ctx_set(change, &old_attrs,
				       &new_attrs, start_page, last_page);
	return 0;
}

static int
amdgpu_svm_attr_set_range(struct amdgpu_svm_attr_tree *attr_tree,
			  const struct amdgpu_svm_attrs *default_attrs,
			  unsigned long start_page, unsigned long last_page,
			  uint32_t nattr,
			  const struct drm_amdgpu_svm_attribute *attrs)
{
	struct amdgpu_svm *svm = attr_tree->svm;
	unsigned long cursor = start_page;
	bool need_retry = false;

	while (cursor <= last_page) {
		struct interval_tree_node *node;
		unsigned long seg_last;
		struct attr_set_ctx change = { 0 };
		int ret;

		amdgpu_svm_lock(svm);
		mutex_lock(&attr_tree->lock);
		node = interval_tree_iter_first(&attr_tree->tree, cursor, cursor);
		if (node) {
			struct amdgpu_svm_attr_range *range;

			range = container_of(node, struct amdgpu_svm_attr_range, it_node);
			seg_last = min(last_page, amdgpu_svm_attr_last_page(range));
			ret = amdgpu_svm_attr_set_existing(attr_tree, range,
								   cursor, seg_last,
								   nattr, attrs, &change);
		} else {
			struct interval_tree_node *next;

			seg_last = last_page;
			if (cursor != ULONG_MAX) {
				next = interval_tree_iter_first(&attr_tree->tree,
								cursor + 1,
								ULONG_MAX);
				if (next) {
					struct amdgpu_svm_attr_range *next_range;

					next_range = container_of(next,
						struct amdgpu_svm_attr_range,
						it_node);
					seg_last = min(last_page,
						       amdgpu_svm_attr_start_page(next_range) - 1);
				}
			}
			ret = amdgpu_svm_attr_set_hole(attr_tree,
							       default_attrs,
							       cursor, seg_last,
							       nattr, attrs,
							       &change);
		}
		mutex_unlock(&attr_tree->lock);

		if (ret) {
			amdgpu_svm_unlock(svm);
			return ret;
		}

		ret = amdgpu_svm_apply_attr_change(svm,
						   &change.old_attrs,
						   &change.new_attrs,
						   change.start_page,
						   change.last_page);
		amdgpu_svm_unlock(svm);

		if (ret == -EAGAIN) {
			need_retry = true;
			ret = 0;
		}

		if (ret)
			return ret;

		if (seg_last == ULONG_MAX || seg_last == last_page)
			break;

		cursor = seg_last + 1;
	}

	return need_retry ? -EAGAIN : 0;
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

int amdgpu_svm_attr_set(struct amdgpu_svm_attr_tree *attr_tree,
			uint64_t start,
			uint64_t size,
			uint32_t nattr,
			const struct drm_amdgpu_svm_attribute *attrs)
{
	struct amdgpu_svm *svm = attr_tree->svm;
	struct amdgpu_svm_attrs default_attrs;
	unsigned long start_page, last_page;
	uint32_t i;
	int r;

	start_page = start >> PAGE_SHIFT;
	last_page = (start + size - 1) >> PAGE_SHIFT;

	for (i = 0; i < nattr; i++) {
		AMDGPU_SVM_TRACE("set attr type %u value 0x%08x range [0x%lx-0x%lx] xnack:%d",
			 attrs[i].type, attrs[i].value, start_page, last_page,
			 svm->xnack_enabled ? 1 : 0);
		r = attr_set_validate(&attrs[i]);
		if (r) {
			AMDGPU_SVM_TRACE("invalid attribute %u value 0x%08x",
					 attrs[i].type, attrs[i].value);
			return r;
		}
	}

	r = amdgpu_svm_attr_validate_range_vma(attr_tree, start_page, last_page);
	if (r)
		return r;

	r = amdgpu_svm_attr_check_vm_bo(attr_tree, start_page, last_page,
					NULL, NULL);
	if (r)
		return r;

	amdgpu_svm_attr_set_default(attr_tree->svm, &default_attrs);

retry:
	r = amdgpu_svm_attr_set_range(attr_tree, &default_attrs,
					       start_page, last_page,
					       nattr, attrs);
	if (r == -EAGAIN) {
		AMDGPU_SVM_TRACE("attr_set retry [0x%lx-0x%lx]\n",
				 start_page, last_page);
		amdgpu_svm_sync_work(svm);
		cond_resched();
		goto retry;
	}

	return r;
}

int amdgpu_svm_attr_clear(struct amdgpu_svm_attr_tree *attr_tree,
			  unsigned long start_page,
			  unsigned long last_page)
{
	struct interval_tree_node *node;
	int r = 0;

	if (start_page > last_page)
		return -EINVAL;

	mutex_lock(&attr_tree->lock);

	node = interval_tree_iter_first(&attr_tree->tree, start_page, last_page);
	while (node) {
		struct interval_tree_node *next;
		struct amdgpu_svm_attr_range *range;
		unsigned long range_start;
		unsigned long range_last;

		range = container_of(node, struct amdgpu_svm_attr_range, it_node);
		next = interval_tree_iter_next(node, start_page, last_page);
		range_start = amdgpu_svm_attr_start_page(range);
		range_last = amdgpu_svm_attr_last_page(range);

		if (range_start < start_page && range_last > last_page) {
			struct amdgpu_svm_attr_range *tail;

			tail = amdgpu_svm_attr_range_alloc(last_page + 1, range_last,
							   &range->attrs);
			if (!tail) {
				r = -ENOMEM;
				break;
			}

			attr_remove_range_locked(attr_tree, range, false);
			attr_set_interval(range, range_start, start_page - 1);
			amdgpu_svm_attr_range_insert_locked(attr_tree, range);
			amdgpu_svm_attr_range_insert_locked(attr_tree, tail);
		} else if (range_start < start_page) {
			attr_remove_range_locked(attr_tree, range, false);
			attr_set_interval(range, range_start, start_page - 1);
			amdgpu_svm_attr_range_insert_locked(attr_tree, range);
		} else if (range_last > last_page) {
			attr_remove_range_locked(attr_tree, range, false);
			attr_set_interval(range, last_page + 1, range_last);
			amdgpu_svm_attr_range_insert_locked(attr_tree, range);
		} else {
			attr_remove_range_locked(attr_tree, range, true);
		}

		node = next;
	}

	mutex_unlock(&attr_tree->lock);
	return r;
}

int amdgpu_svm_attr_reset(struct amdgpu_svm_attr_tree *attr_tree,
			  unsigned long start_page,
			  unsigned long last_page)
{
	/*
	 * Range with no attr node in this implementation is treated the
	 * same as one that explicitly stores default attrs. So clear
	 * the attribute ranges when user wants to reset the attrs.
	 *
	 * - GET_ATTR: return default_attrs to userspace when there is no attr
	 *   range.
	 * - SET_ATTR: holes are treated as having default_attrs when
	 *   computing the change trigger.
	 * - Fault: attr lookup falls back to defaults when no attr range exists,
	 *   so migration and PTE flag decisions are unchanged.
	 *
	 * This approach simplifies the implementation and avoids redundant
	 * attribute maintenance. This ioctl operation is for attribute only,
	 * so do not invalidate the GPU mapping here.
	 */
	return amdgpu_svm_attr_clear(attr_tree, start_page, last_page);
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
		ctx->access = AMDGPU_SVM_ACCESS_INACCESSIBLE;
	ctx->flags_and &= attrs->flags;
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
			attrs[i].value = (uint32_t)ctx->access;
			break;
		case AMDGPU_SVM_ATTR_HOST_ACCESS:
		case AMDGPU_SVM_ATTR_COHERENT:
		case AMDGPU_SVM_ATTR_HIVE_LOCAL:
		case AMDGPU_SVM_ATTR_GPU_RO:
		case AMDGPU_SVM_ATTR_GPU_EXEC:
		case AMDGPU_SVM_ATTR_GPU_READ_MOSTLY:
		case AMDGPU_SVM_ATTR_EXT_COHERENT: {
			uint32_t flag_bit;

			if (!attr_flag_type_to_bit(attrs[i].type, &flag_bit))
				return -EINVAL;

			attrs[i].value = (ctx->flags_and & flag_bit) ? 1 : 0;
			break;
		}
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

	r = amdgpu_svm_attr_validate_range_vma(attr_tree, start_page, last_page);
	if (r)
		return r;

	r = amdgpu_svm_attr_check_vm_bo(attr_tree, start_page, last_page,
					NULL, NULL);
	if (r)
		return r;

	mutex_lock(&attr_tree->lock);
	amdgpu_svm_attr_set_default(attr_tree->svm, &default_attrs);
	node = interval_tree_iter_first(&attr_tree->tree, start_page, last_page);

	if (!node) {
		attr_get_ctx_add(&ctx, &default_attrs);
		r = attr_get_ctx_to_result(&ctx, nattr, attrs);
		mutex_unlock(&attr_tree->lock);
		return r;
	}

	cursor = start_page;
	while (cursor <= last_page) {
		const struct amdgpu_svm_attrs *range_attrs;
		unsigned long range_last = last_page;
		struct amdgpu_svm_attr_range *range = NULL;
		unsigned long next;

		if (node) {
			range = container_of(node, struct amdgpu_svm_attr_range,
					     it_node);

			if (amdgpu_svm_attr_last_page(range) < cursor) {
				node = interval_tree_iter_next(node, start_page,
							      last_page);
				continue;
			}

			if (amdgpu_svm_attr_start_page(range) <= cursor) {
				range_last = min(last_page, amdgpu_svm_attr_last_page(range));
				node = interval_tree_iter_next(node, start_page,
							      last_page);
			} else {
				range_last = min(last_page,
						 amdgpu_svm_attr_start_page(range) - 1);
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
