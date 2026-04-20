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


/* one fd one svm one GPU so no bit map
 * only three status for this pattren.
 */
enum amdgpu_svm_attr_access {
	AMDGPU_SVM_ACCESS_NONE = 0,
	AMDGPU_SVM_ACCESS_ENABLE = 1,
	AMDGPU_SVM_ACCESS_IN_PLACE = 2,
};

#define AMDGPU_SVM_PTE_FLAG_MASK \
	(AMDGPU_SVM_FLAG_COHERENT | AMDGPU_SVM_FLAG_EXT_COHERENT | \
	 AMDGPU_SVM_FLAG_GPU_RO | AMDGPU_SVM_FLAG_GPU_EXEC)

#define AMDGPU_SVM_MAPPING_FLAG_MASK \
	(AMDGPU_SVM_FLAG_HOST_ACCESS | AMDGPU_SVM_FLAG_HIVE_LOCAL | \
	 AMDGPU_SVM_FLAG_GPU_READ_MOSTLY | AMDGPU_SVM_FLAG_GPU_ALWAYS_MAPPED)

struct amdgpu_svm_attrs {
	/* keep preferred_loc to adapt to kfd API */
	int32_t preferred_loc;
	int32_t prefetch_loc;
	uint32_t flags;
	uint32_t granularity;
	enum amdgpu_svm_attr_access access;
};

struct amdgpu_svm_attr_range {
	struct interval_tree_node it_node;
	struct list_head list;
	struct amdgpu_svm_attrs attrs;
};

struct amdgpu_svm;

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
	AMDGPU_SVM_ATTR_TRIGGER_ATTR_ONLY = (1U << 5), 				/* no changes */
};

struct amdgpu_svm_attr_tree *
amdgpu_svm_attr_tree_create(struct amdgpu_svm *svm);
void amdgpu_svm_attr_tree_destroy(struct amdgpu_svm_attr_tree *attr_tree);
int amdgpu_svm_attr_cache_init(void);
void amdgpu_svm_attr_cache_fini(void);
void amdgpu_svm_attr_lookup_page_locked(struct amdgpu_svm_attr_tree *attr_tree,
						  unsigned long page,
						  struct amdgpu_svm_attrs *attrs,
						  unsigned long *seg_last);

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
int amdgpu_svm_attr_clear_pages(struct amdgpu_svm_attr_tree *attr_tree,
				unsigned long start_page,
				unsigned long last_page);

#endif /* __AMDGPU_SVM_ATTR_H__ */
