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

#ifndef __AMDGPU_SVM_RANGE_H__
#define __AMDGPU_SVM_RANGE_H__

#include <drm/drm_gpusvm.h>

#include <linux/list.h>
#include <linux/types.h>

struct amdgpu_svm;
struct amdgpu_svm_attrs;
struct drm_gpusvm_notifier;
struct drm_gpusvm_range;
struct mmu_notifier_range;

struct amdgpu_svm_range {
	struct drm_gpusvm_range base;
	struct list_head gc_node;
	bool gpu_mapped;
	bool gc_queued;
	bool restore_queued;
	bool in_queue;
	u8 pending_ops;
	unsigned long pending_start;
	unsigned long pending_last;
	uint64_t pte_flags;
	uint32_t attr_flags;
};

static inline struct amdgpu_svm_range *
to_amdgpu_svm_range(struct drm_gpusvm_range *range)
{
	return container_of(range, struct amdgpu_svm_range, base);
}

int amdgpu_svm_range_work_init(struct amdgpu_svm *svm);
void amdgpu_svm_range_work_fini(struct amdgpu_svm *svm);
void amdgpu_svm_range_flush(struct amdgpu_svm *svm);
void amdgpu_svm_range_sync_work(struct amdgpu_svm *svm);
int amdgpu_svm_range_map_attr_ranges(struct amdgpu_svm *svm,
				     unsigned long start_page,
				     unsigned long last_page);
int amdgpu_svm_range_apply_attr_change(
	struct amdgpu_svm *svm, unsigned long start, unsigned long last,
	uint32_t trigger, const struct amdgpu_svm_attrs *prev_attrs,
	const struct amdgpu_svm_attrs *new_attrs);
void amdgpu_svm_range_invalidate(struct amdgpu_svm *svm,
				 struct drm_gpusvm_notifier *notifier,
				 const struct mmu_notifier_range *mmu_range);
void amdgpu_svm_range_restore_begin_compute(struct amdgpu_svm *svm);
void amdgpu_svm_range_restore_end_compute(struct amdgpu_svm *svm);

#endif /* __AMDGPU_SVM_RANGE_H__ */
