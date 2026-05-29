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

#ifndef __AMDGPU_SVM_ATTR_H__
#define __AMDGPU_SVM_ATTR_H__

#include <drm/amdgpu_drm.h>
#include <linux/interval_tree.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/types.h>

/* Internal SVM attribute bitmask flags mapped from UAPI ioctl definitions */
#define AMDGPU_SVM_ATTR_BIT_HOST_ACCESS		(1u << 0)
#define AMDGPU_SVM_ATTR_BIT_COHERENT		(1u << 1)
#define AMDGPU_SVM_ATTR_BIT_EXT_COHERENT	(1u << 2)
#define AMDGPU_SVM_ATTR_BIT_HIVE_LOCAL		(1u << 3)
#define AMDGPU_SVM_ATTR_BIT_GPU_RO		(1u << 4)
#define AMDGPU_SVM_ATTR_BIT_GPU_EXEC		(1u << 5)
#define AMDGPU_SVM_ATTR_BIT_GPU_READ_MOSTLY	(1u << 6)

#define AMDGPU_SVM_PTE_FLAG_MASK \
	(AMDGPU_SVM_ATTR_BIT_COHERENT | AMDGPU_SVM_ATTR_BIT_EXT_COHERENT | \
	 AMDGPU_SVM_ATTR_BIT_GPU_RO | AMDGPU_SVM_ATTR_BIT_GPU_EXEC)

#define AMDGPU_SVM_MAPPING_FLAG_MASK \
	(AMDGPU_SVM_ATTR_BIT_HOST_ACCESS | AMDGPU_SVM_ATTR_BIT_HIVE_LOCAL | \
	 AMDGPU_SVM_ATTR_BIT_GPU_READ_MOSTLY)

struct amdgpu_svm_attrs {
	/* keep preferred_loc to adapt to kfd API */
	int32_t preferred_loc;
	int32_t prefetch_loc;
	uint32_t flags;
	uint32_t granularity;
	enum amdgpu_ioctl_svm_access access;
};

struct amdgpu_svm_attr_range {
	struct interval_tree_node it_node;
	struct list_head list;
	struct amdgpu_svm_attrs attrs;
};

static inline unsigned long
amdgpu_svm_attr_start_page(const struct amdgpu_svm_attr_range *range)
{
	return range->it_node.start;
}

static inline unsigned long
amdgpu_svm_attr_last_page(const struct amdgpu_svm_attr_range *range)
{
	return range->it_node.last;
}

static inline unsigned long
amdgpu_svm_attr_start(const struct amdgpu_svm_attr_range *range)
{
	return range->it_node.start << PAGE_SHIFT;
}

static inline unsigned long
amdgpu_svm_attr_end(const struct amdgpu_svm_attr_range *range)
{
	return (range->it_node.last + 1) << PAGE_SHIFT;
}

struct amdgpu_svm;
struct mm_struct;
struct vm_area_struct;

static inline bool
amdgpu_svm_attr_has_access(enum amdgpu_ioctl_svm_access access)
{
	return access == AMDGPU_SVM_ACCESS_ALLOW_MIGRATE ||
	       access == AMDGPU_SVM_ACCESS_IN_PLACE;
}

struct amdgpu_svm_attr_tree {
	struct mutex lock;
	struct rb_root_cached tree;
	struct list_head range_list;
	struct amdgpu_svm *svm;
};

enum amdgpu_svm_attr_change_trigger {
	AMDGPU_SVM_ATTR_TRIGGER_ACCESS_CHANGE = (1U << 0),
	AMDGPU_SVM_ATTR_TRIGGER_PTE_FLAG_CHANGE = (1U << 1),
	AMDGPU_SVM_ATTR_TRIGGER_MAPPING_FLAG_CHANGE = (1U << 2),
	AMDGPU_SVM_ATTR_TRIGGER_LOCATION_CHANGE = (1U << 3),
	AMDGPU_SVM_ATTR_TRIGGER_GRANULARITY_CHANGE = (1U << 4),
	AMDGPU_SVM_ATTR_TRIGGER_PREFETCH = (1U << 5),
};

#define AMDGPU_SVM_ATTR_TRIGGER_NEED_INVALIDATE \
	(AMDGPU_SVM_ATTR_TRIGGER_ACCESS_CHANGE | \
	 AMDGPU_SVM_ATTR_TRIGGER_PTE_FLAG_CHANGE | \
	 AMDGPU_SVM_ATTR_TRIGGER_MAPPING_FLAG_CHANGE | \
	 AMDGPU_SVM_ATTR_TRIGGER_LOCATION_CHANGE)

struct amdgpu_svm_attr_tree *
amdgpu_svm_attr_tree_create(struct amdgpu_svm *svm);
void amdgpu_svm_attr_tree_destroy(struct amdgpu_svm_attr_tree *attr_tree);
int amdgpu_svm_attr_cache_init(void);
void amdgpu_svm_attr_cache_fini(void);
struct amdgpu_svm_attr_range *
amdgpu_svm_attr_find_locked(struct amdgpu_svm_attr_tree *attr_tree,
			   unsigned long page);
struct amdgpu_svm_attr_range *
amdgpu_svm_attr_get_bounds_locked(struct amdgpu_svm_attr_tree *attr_tree,
				  unsigned long page,
				  unsigned long *start_page,
				  unsigned long *last_page);
void amdgpu_svm_attr_set_default(struct amdgpu_svm *svm,
				 struct amdgpu_svm_attrs *attrs);

int amdgpu_svm_attr_set(struct amdgpu_svm_attr_tree *attr_tree,
			   uint64_t start,
			   uint64_t size,
			   uint32_t nattr,
			   const struct drm_amdgpu_svm_attribute *attrs);
int amdgpu_svm_attr_get(struct amdgpu_svm_attr_tree *attr_tree,
				       uint64_t start,
				       uint64_t size,
				       uint32_t nattr,
				       struct drm_amdgpu_svm_attribute *attrs);
int amdgpu_svm_attr_clear(struct amdgpu_svm_attr_tree *attr_tree,
			  unsigned long start_page,
			  unsigned long last_page);
int amdgpu_svm_attr_reset(struct amdgpu_svm_attr_tree *attr_tree,
			  unsigned long start_page,
			  unsigned long last_page);
struct amdgpu_svm_attr_range *
amdgpu_svm_attr_range_alloc(unsigned long start_page,
			   unsigned long last_page,
			   const struct amdgpu_svm_attrs *attrs);
void amdgpu_svm_attr_range_insert_locked(struct amdgpu_svm_attr_tree *attr_tree,
					 struct amdgpu_svm_attr_range *range);
bool amdgpu_svm_attr_devmem_possible(struct amdgpu_svm *svm,
				     const struct amdgpu_svm_attrs *attrs);
bool amdgpu_svm_attr_prefer_vram(struct amdgpu_svm *svm,
				 const struct amdgpu_svm_attrs *attrs);
struct vm_area_struct *amdgpu_svm_check_vma(struct mm_struct *mm,
					unsigned long addr);
int amdgpu_svm_attr_check_vm_bo(struct amdgpu_svm_attr_tree *attr_tree,
				unsigned long start_page,
				unsigned long last_page,
				unsigned long *bo_start,
				unsigned long *bo_last);

#endif /* __AMDGPU_SVM_ATTR_H__ */
