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

#define AMDGPU_SVM_VALID_FLAG_MASK \
	(AMDGPU_SVM_FLAG_HOST_ACCESS | AMDGPU_SVM_FLAG_COHERENT | \
	 AMDGPU_SVM_FLAG_HIVE_LOCAL | AMDGPU_SVM_FLAG_GPU_RO | \
	 AMDGPU_SVM_FLAG_GPU_EXEC | AMDGPU_SVM_FLAG_GPU_READ_MOSTLY | \
	 AMDGPU_SVM_FLAG_GPU_ALWAYS_MAPPED | AMDGPU_SVM_FLAG_EXT_COHERENT)


static struct kmem_cache *amdgpu_svm_attr_range_cache;

struct attr_set_ctx {
	unsigned long start;
	unsigned long last;
	uint32_t trigger;
	struct amdgpu_svm_attrs prev_attrs;
	struct amdgpu_svm_attrs new_attrs;
};

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

static uint32_t
attr_change_ctx_trigger(const struct amdgpu_svm_attrs *prev_attrs,
		      const struct amdgpu_svm_attrs *new_attrs)
{
	uint32_t trigger = 0;
	uint32_t changed_flags = prev_attrs->flags ^ new_attrs->flags;

	if (prev_attrs->access != new_attrs->access)
		trigger |= AMDGPU_SVM_ATTR_TRIGGER_ACCESS_CHANGE;

	if (changed_flags & AMDGPU_SVM_PTE_FLAG_MASK)
		trigger |= AMDGPU_SVM_ATTR_TRIGGER_PTE_FLAG_CHANGE;
	if (changed_flags & AMDGPU_SVM_MAPPING_FLAG_MASK)
		trigger |= AMDGPU_SVM_ATTR_TRIGGER_MAPPING_FLAG_CHANGE;
	if (prev_attrs->preferred_loc != new_attrs->preferred_loc ||
	    prev_attrs->prefetch_loc != new_attrs->prefetch_loc)
		trigger |= AMDGPU_SVM_ATTR_TRIGGER_LOCATION_CHANGE;
	if (prev_attrs->granularity != new_attrs->granularity)
		trigger |= AMDGPU_SVM_ATTR_TRIGGER_GRANULARITY_CHANGE;

	if (!trigger)
		trigger = AMDGPU_SVM_ATTR_TRIGGER_ATTR_ONLY;

	return trigger;
}

static bool attr_has_access(uint32_t nattr,
					  const struct drm_amdgpu_svm_attribute *attrs)
{
	uint32_t i;

	for (i = 0; i < nattr; i++) {
		switch (attrs[i].type) {
		case AMDGPU_SVM_ATTR_ACCESS:
		case AMDGPU_SVM_ATTR_ACCESS_IN_PLACE:
			return true;
		}
	}

	return false;
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

static void amdgpu_svm_attr_change_ctx_set(
		struct attr_set_ctx *change,
		unsigned long start,
		unsigned long last,
		uint32_t trigger,
		const struct amdgpu_svm_attrs *prev_attrs,
		const struct amdgpu_svm_attrs *new_attrs)
{
	change->start = start;
	change->last = last;
	change->trigger = trigger;
	change->prev_attrs = *prev_attrs;
	change->new_attrs = *new_attrs;
}

static int amdgpu_svm_attr_apply_change(
				struct amdgpu_svm *svm,
				const struct attr_set_ctx *change)
{
	int ret;

	lockdep_assert_held_write(&svm->svm_lock);

	if (!change->trigger ||
	    change->trigger == AMDGPU_SVM_ATTR_TRIGGER_ATTR_ONLY)
		return 0;

	ret = amdgpu_svm_range_apply_attr_change(svm, change->start, change->last,
						 change->trigger, &change->prev_attrs,
						 &change->new_attrs);
	if (ret)
		AMDGPU_SVM_TRACE("mapping apply failed ret=%d [0x%lx-0x%lx]-0x%lx trigger=0x%x\n",
				 ret, change->start, change->last,
				 change->last - change->start + 1,
				 change->trigger);

	return ret;
}

static inline int attr_check_preferred_loc(uint32_t value)
{
	/* casue one svm one gpu so value > 0 then means prefered loc is this GPU */
	if (value == AMDGPU_SVM_LOCATION_SYSMEM || value == AMDGPU_SVM_LOCATION_UNDEFINED)
		return 0;

	return 0;
}

static inline int attr_check_prefetch_loc(uint32_t value)
{
	/* casue one svm one gpu so value > 0 then means prefetch loc is this GPU 
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
	if (!value || value == AMDGPU_SVM_LOCATION_UNDEFINED)
		return -EINVAL;

	return 0;
}

static inline int attr_check_flags(uint32_t value)
{
	if (value & ~AMDGPU_SVM_VALID_FLAG_MASK)
		return -EINVAL;

	return 0;
}

static inline int attr_check_granularity(uint32_t value)
{
	return 0;
}

static int
amdgpu_svm_attr_validate_range_vma(struct amdgpu_svm_attr_tree *attr_tree,
				   unsigned long start_page,
				   unsigned long last_page)
{
	const unsigned long device_vma = VM_IO | VM_PFNMAP | VM_MIXEDMAP;
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
		struct vm_area_struct *vma = vma_lookup(mm, start);

		if (!vma || (vma->vm_flags & device_vma)) {
			ret = -EFAULT;
			break;
		}

		start = min(end, vma->vm_end);
	}
	mmap_read_unlock(mm);

	return ret;
}

static int amdgpu_svm_attr_set_validate(const struct drm_amdgpu_svm_attribute *attr)
{
	switch (attr->type) {
	case AMDGPU_SVM_ATTR_PREFERRED_LOC:
		return attr_check_preferred_loc(attr->value);
	case AMDGPU_SVM_ATTR_PREFETCH_LOC:
		return attr_check_prefetch_loc(attr->value);
	case AMDGPU_SVM_ATTR_ACCESS:
	case AMDGPU_SVM_ATTR_ACCESS_IN_PLACE:
	case AMDGPU_SVM_ATTR_NO_ACCESS:
		return attr_check_access(attr->value);
	case AMDGPU_SVM_ATTR_SET_FLAGS:
	case AMDGPU_SVM_ATTR_CLR_FLAGS:
		return attr_check_flags(attr->value);
	case AMDGPU_SVM_ATTR_GRANULARITY:
		return attr_check_granularity(attr->value);
	default:
		return -EINVAL;
	}
}

static void amdgpu_svm_attr_apply(struct amdgpu_svm_attrs *attrs,
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
			attrs->access = AMDGPU_SVM_ACCESS_ENABLE;
			break;
		case AMDGPU_SVM_ATTR_ACCESS_IN_PLACE:
			attrs->access = AMDGPU_SVM_ACCESS_IN_PLACE;
			break;
		case AMDGPU_SVM_ATTR_NO_ACCESS:
			attrs->access = AMDGPU_SVM_ACCESS_NONE;
			break;
		case AMDGPU_SVM_ATTR_SET_FLAGS:
			attrs->flags |= attr->value;
			break;
		case AMDGPU_SVM_ATTR_CLR_FLAGS:
			attrs->flags &= ~attr->value;
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
	amdgpu_svm_attr_apply(&target, nattr, attrs);
	return amdgpu_svm_attr_equal(&range->attrs, &target);
}

static int
amdgpu_svm_attr_set_hole(struct amdgpu_svm_attr_tree *attr_tree,
			  const struct amdgpu_svm_attrs *default_attrs,
			  unsigned long start, unsigned long last,
			  uint32_t nattr,
			  const struct drm_amdgpu_svm_attribute *attrs,
			  struct attr_set_ctx *change)
{
	struct amdgpu_svm_attrs new_attrs;
	struct amdgpu_svm_attr_range *range;
	uint32_t trigger;

	lockdep_assert_held(&attr_tree->lock);

	if (start > last)
		return 0;

	/* no action if default attr */
	new_attrs = *default_attrs;
	amdgpu_svm_attr_apply(&new_attrs, nattr, attrs);
	if (amdgpu_svm_attr_equal(default_attrs, &new_attrs))
		return 0;

	range = attr_alloc_range(start, last, &new_attrs);
	if (!range)
		return -ENOMEM;

	attr_insert_range_locked(attr_tree, range);

	trigger = attr_change_ctx_trigger(default_attrs, &new_attrs);
	amdgpu_svm_attr_change_ctx_set(change, start, last, trigger,
					   default_attrs, &new_attrs);
	return 0;
}

static int
amdgpu_svm_attr_set_existing(struct amdgpu_svm_attr_tree *attr_tree,
			     struct amdgpu_svm_attr_range *range,
			     unsigned long start, unsigned long last,
			     uint32_t nattr,
			     const struct drm_amdgpu_svm_attribute *attrs,
			     struct attr_set_ctx *change)
{
	unsigned long range_start = attr_start_page(range);
	unsigned long range_last = attr_last_page(range);
	struct amdgpu_svm_attr_range *left = NULL;
	struct amdgpu_svm_attr_range *right = NULL;
	struct amdgpu_svm_attrs old_attrs;
	struct amdgpu_svm_attrs new_attrs;
	uint32_t trigger;
	bool force_trigger;

	lockdep_assert_held(&attr_tree->lock);

	old_attrs = range->attrs;

	/* The attr layer doesn't store the gpu mapped state, and for align with KFD,
	 * need force trigger range layer to check if gpu mapped.
	 */
	force_trigger = !attr_tree->svm->xnack_enabled && attr_has_access(nattr, attrs);

	if (attr_same_attrs(range, nattr, attrs)) {
		if (!force_trigger)
			return 0;

		amdgpu_svm_attr_change_ctx_set(change, start, last,
						   AMDGPU_SVM_ATTR_TRIGGER_ACCESS_CHANGE,
						   &old_attrs, &old_attrs);
		return 0;
	}

	new_attrs = old_attrs;
	amdgpu_svm_attr_apply(&new_attrs, nattr, attrs);
	trigger = attr_change_ctx_trigger(&old_attrs, &new_attrs);

	/* only need to update attr */
	if (start == range_start && last == range_last) {
		range->attrs = new_attrs;
		amdgpu_svm_attr_change_ctx_set(change, start, last,
						   trigger, &old_attrs, &new_attrs);
		return 0;
	}

	/* split head */
	if (start > range_start) {
		left = attr_alloc_range(range_start, start - 1, &old_attrs);
		if (!left)
			return -ENOMEM;
	}

	/* split tail */
	if (last < range_last) {
		right = attr_alloc_range(last + 1, range_last, &old_attrs);
		if (!right) {
			if (left)
				kmem_cache_free(amdgpu_svm_attr_range_cache, left);
			return -ENOMEM;
		}
	}

	attr_remove_range_locked(attr_tree, range, false);
	if (left)
		attr_insert_range_locked(attr_tree, left);
	attr_set_interval(range, start, last);
	range->attrs = new_attrs;
	attr_insert_range_locked(attr_tree, range);
	if (right)
		attr_insert_range_locked(attr_tree, right);

	amdgpu_svm_attr_change_ctx_set(change, start, last, trigger,
					   &old_attrs, &new_attrs);
	return 0;
}

static int
amdgpu_svm_attr_set_range(struct amdgpu_svm_attr_tree *attr_tree,
			  const struct amdgpu_svm_attrs *default_attrs,
			  unsigned long start, unsigned long last,
			  uint32_t nattr,
			  const struct drm_amdgpu_svm_attribute *attrs)
{
	struct amdgpu_svm *svm = attr_tree->svm;
	unsigned long cursor = start;
	bool need_retry = false;

	while (cursor <= last) {
		struct interval_tree_node *node;
		unsigned long seg_last;
		struct attr_set_ctx change = { 0 };
		int ret;

		mutex_lock(&attr_tree->lock);
		node = interval_tree_iter_first(&attr_tree->tree, cursor, cursor);
		if (node) {
			struct amdgpu_svm_attr_range *range;

			range = container_of(node, struct amdgpu_svm_attr_range, it_node);
			seg_last = min(last, attr_last_page(range));
			ret = amdgpu_svm_attr_set_existing(attr_tree, range,
								   cursor, seg_last,
								   nattr, attrs, &change);
		} else {
			struct interval_tree_node *next;

			seg_last = last;
			if (cursor != ULONG_MAX) {
				next = interval_tree_iter_first(&attr_tree->tree,
								cursor + 1,
								ULONG_MAX);
				if (next) {
					struct amdgpu_svm_attr_range *next_range;

					next_range = container_of(next,
						struct amdgpu_svm_attr_range,
						it_node);
					seg_last = min(last,
						       attr_start_page(next_range) - 1);
				}
			}
			ret = amdgpu_svm_attr_set_hole(attr_tree,
							       default_attrs,
							       cursor, seg_last,
							       nattr, attrs,
							       &change);
		}
		mutex_unlock(&attr_tree->lock);

		if (ret)
			return ret;

		down_write(&svm->svm_lock);
		ret = amdgpu_svm_attr_apply_change(svm, &change);
		up_write(&svm->svm_lock);

		if (ret == -EAGAIN) {
			need_retry = true;
			ret = 0;
		}

		if (ret)
			return ret;

		if (seg_last == ULONG_MAX || seg_last == last)
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
		AMDGPU_SVM_TRACE("set attr type %u value 0x%08x for page range [%lx, %lx] xnack:%d", 
			attrs[i].type, attrs[i].value, start_page, last_page, svm->xnack_enabled ? 1 : 0);
		r = amdgpu_svm_attr_set_validate(&attrs[i]);
		if (r) {
			AMDGPU_SVM_TRACE("invalid attribute %u value 0x%08x", attrs[i].type, attrs[i].value);
			return r;
		}
	}

	r = amdgpu_svm_attr_validate_range_vma(attr_tree, start_page, last_page);
	if (r)
		return r;

	attr_set_default(attr_tree->svm, &default_attrs);

	/*
	 * POC/WA:
	 * can not acquire the mmap lock because of drm gpu svm frame work design (drm_gpusvm_range_find_or_insert)
	 * the hmm operations and GPU mapping possiable to fail so add retry mechanism
	 * 
	 * TODO: add mmap locked flag in drm_gpusvm_ctx to acquire mmap lock in entire ioctl period
	 */
retry:
	r = amdgpu_svm_attr_set_range(attr_tree, &default_attrs,
					       start_page, last_page,
					       nattr, attrs);
	if (r == -EAGAIN) {
		AMDGPU_SVM_TRACE("attr_set retry [0x%lx-0x%lx]\n",
				 start_page, last_page);
		amdgpu_svm_range_flush(svm);
		cond_resched();
		goto retry;
	}

	return r;
}

int amdgpu_svm_attr_clear_pages(struct amdgpu_svm_attr_tree *attr_tree,
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
		range_start = attr_start_page(range);
		range_last = attr_last_page(range);

		if (range_start < start_page && range_last > last_page) {
			struct amdgpu_svm_attr_range *tail;

			tail = attr_alloc_range(last_page + 1, range_last, &range->attrs);
			if (!tail) {
				r = -ENOMEM;
				break;
			}

			attr_remove_range_locked(attr_tree, range, false);
			attr_set_interval(range, range_start, start_page - 1);
			attr_insert_range_locked(attr_tree, range);
			attr_insert_range_locked(attr_tree, tail);
		} else if (range_start < start_page) {
			attr_remove_range_locked(attr_tree, range, false);
			attr_set_interval(range, range_start, start_page - 1);
			attr_insert_range_locked(attr_tree, range);
		} else if (range_last > last_page) {
			attr_remove_range_locked(attr_tree, range, false);
			attr_set_interval(range, last_page + 1, range_last);
			attr_insert_range_locked(attr_tree, range);
		} else {
			attr_remove_range_locked(attr_tree, range, true);
		}

		node = next;
	}

	mutex_unlock(&attr_tree->lock);
	return r;
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
