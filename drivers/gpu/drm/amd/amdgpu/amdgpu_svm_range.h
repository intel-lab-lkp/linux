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
#include <drm/drm_pagemap.h>

#include "amdgpu_svm.h"
#include "amdgpu_vm.h"

#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/types.h>

struct amdgpu_svm;
struct amdgpu_svm_attr_range;
struct amdgpu_svm_attrs;
struct dma_fence;
struct drm_exec;
struct drm_gpusvm_notifier;
struct drm_gpusvm_range;
struct mmu_notifier_range;

enum amdgpu_svm_range_queue_state {
	AMDGPU_SVM_RANGE_NOT_QUEUED = 0,
	AMDGPU_SVM_RANGE_IN_GC,
	AMDGPU_SVM_RANGE_IN_RESTORE,
	AMDGPU_SVM_RANGE_PROCESSING,
};

struct amdgpu_svm_range {
	struct drm_gpusvm_range base;
	struct list_head work_node;
	bool gpu_mapped;
	u8 queue_state;
	u8 pending_ops;
	unsigned long pending_start_page;
	unsigned long pending_last_page;
	uint32_t attr_flags;
	ktime_t validate_timestamp;
};

static inline struct amdgpu_svm_range *
to_amdgpu_svm_range(struct drm_gpusvm_range *range)
{
	return container_of(range, struct amdgpu_svm_range, base);
}

static inline void
amdgpu_svm_range_invalidate_gpu_mapping(struct amdgpu_svm_range *range)
{
	WRITE_ONCE(range->gpu_mapped, false);
}

#define AMDGPU_SVM_RANGE_DEBUG(r__, op__)                                      \
	AMDGPU_SVM_TRACE("%s: pasid=%u, gpusvm=%p, mapped=%d, "                \
			 "seqno=%lu, range: [0x%lx-0x%lx]-"                    \
			 "0x%lx\n",                                            \
			 (op__), to_amdgpu_svm((r__)->base.gpusvm)->vm->pasid, \
			 (r__)->base.gpusvm, READ_ONCE((r__)->gpu_mapped),     \
			 (r__)->base.pages.notifier_seq,                       \
			 drm_gpusvm_range_start(&(r__)->base) >> PAGE_SHIFT,   \
			 drm_gpusvm_range_end(&(r__)->base) >> PAGE_SHIFT,     \
			 (drm_gpusvm_range_end(&(r__)->base) -                 \
			  drm_gpusvm_range_start(&(r__)->base)) >> PAGE_SHIFT)

enum amdgpu_svm_range_op {
	AMDGPU_SVM_RANGE_OP_NONE    = 0,
	AMDGPU_SVM_RANGE_OP_UNMAP   = BIT(0),
};

struct amdgpu_svm_range_op_ctx {
	struct amdgpu_svm_range *range;
	unsigned long start_page;
	unsigned long last_page;
	uint8_t pending_ops;
};

#define UNMAP_WORK(ops)		((ops) & AMDGPU_SVM_RANGE_OP_UNMAP)

void amdgpu_svm_capture_checkpoint_ts(struct amdgpu_svm *svm);

uint64_t amdgpu_svm_range_attr_pte_flags(struct amdgpu_svm *svm,
					 const struct amdgpu_svm_attrs *attrs,
					 bool read_only,
					 enum drm_interconnect_protocol proto);
int amdgpu_svm_range_lock_vm_pd(struct amdgpu_svm *svm,
				struct drm_exec *exec,
				bool intr);
bool amdgpu_svm_range_pages_valid(struct amdgpu_svm *svm,
				  struct amdgpu_svm_range *range);
bool amdgpu_svm_range_is_valid(struct amdgpu_svm *svm,
			       struct amdgpu_svm_range *range,
			       const struct amdgpu_svm_attrs *attrs);
int amdgpu_svm_range_update_gpu_range(struct amdgpu_svm *svm,
				      struct amdgpu_svm_range *range,
				      const struct amdgpu_svm_attrs *attrs,
				      bool read_only,
				      bool flush_tlb, bool wait,
				      struct dma_fence **fence);
int amdgpu_svm_range_update_mapping(struct amdgpu_svm *svm,
				    struct amdgpu_svm_range *range,
				    const struct amdgpu_svm_attrs *attrs,
				    bool read_only,
				    bool intr, bool wait,
				    bool flush_tlb);
bool amdgpu_svm_range_dequeue_locked(struct amdgpu_svm *svm,
				     struct list_head *work_list,
				     struct amdgpu_svm_range_op_ctx *op_ctx);
void amdgpu_svm_range_put_if_dequeued(struct amdgpu_svm *svm,
				      struct amdgpu_svm_range *range);
void amdgpu_svm_range_remove(struct amdgpu_svm *svm,
			     struct amdgpu_svm_range *range,
			     struct drm_gpusvm_ctx *ctx);
int amdgpu_svm_range_map_attrs(struct amdgpu_svm *svm,
			       const struct amdgpu_svm_attrs *attrs,
			       unsigned long start, unsigned long end);
int amdgpu_svm_range_invalidate_interval(struct amdgpu_svm *svm,
					 unsigned long start_page,
					 unsigned long last_page);
int amdgpu_svm_range_zap_ptes(struct amdgpu_svm *svm,
			      struct amdgpu_svm_range *range,
			      unsigned long start_page,
			      unsigned long last_page);
void amdgpu_svm_range_evict(struct amdgpu_svm *svm,
			    struct drm_gpusvm_range *range);
void amdgpu_svm_range_invalidate(struct amdgpu_svm *svm,
				 struct drm_gpusvm_notifier *notifier,
				 const struct mmu_notifier_range *mmu_range,
				 struct drm_gpusvm_range *first,
				 uint64_t adj_start, uint64_t adj_end);
bool amdgpu_svm_range_notifier_event_begin(struct amdgpu_svm *svm,
					   struct drm_gpusvm_range *range,
					   const struct mmu_notifier_range *mmu_range);
struct amdgpu_svm_range *
amdgpu_svm_range_find_or_insert(struct amdgpu_svm *svm, unsigned long addr,
				unsigned long gpuva_start, unsigned long gpuva_end,
				struct drm_gpusvm_ctx *ctx);
int amdgpu_svm_range_get_pages(struct amdgpu_svm *svm,
			       struct drm_gpusvm_range *range,
			       struct drm_gpusvm_ctx *ctx);

#endif /* __AMDGPU_SVM_RANGE_H__ */
