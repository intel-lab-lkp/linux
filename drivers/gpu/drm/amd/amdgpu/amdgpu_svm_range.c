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
#include "amdgpu.h"
#include "amdgpu_amdkfd.h"
#include "amdgpu_vm.h"

#include <drm/drm_exec.h>
#include <drm/drm_pagemap.h>

#include <linux/mmu_notifier.h>
#include <uapi/linux/kfd_ioctl.h>

enum amdgpu_svm_range_queue_op {
	AMDGPU_SVM_RANGE_OP_RESTORE = 0,
	AMDGPU_SVM_RANGE_OP_UNMAP = 1,
};

enum amdgpu_svm_range_pending_op {
	AMDGPU_SVM_RANGE_PENDING_OP_NONE    = 0,
	AMDGPU_SVM_RANGE_PENDING_OP_UNMAP   = BIT(0),
	AMDGPU_SVM_RANGE_PENDING_OP_RESTORE = BIT(1),
};

#define UNMAP_WORK(ops) ((ops) & AMDGPU_SVM_RANGE_PENDING_OP_UNMAP)

#define RESTORE_WORK(ops) ((ops) & AMDGPU_SVM_RANGE_PENDING_OP_RESTORE)

#define NEED_REBUILD(svm) (!(svm)->xnack_enabled)

enum amdgpu_svm_range_notifier_op {
	AMDGPU_SVM_RANGE_NOTIFIER_CLEAR_PTE = BIT(0),
	AMDGPU_SVM_RANGE_NOTIFIER_QUEUE_INTERVAL = BIT(1),
};

struct range_pending_op_ctx {
	struct amdgpu_svm_range *range;
	unsigned long start;
	unsigned long last;
	uint8_t pending_ops;
};

#define AMDGPU_SVM_RANGE_RESTORE_DELAY_MS 1
#define AMDGPU_SVM_RANGE_WQ_NAME "amdgpu_svm_range"
#define AMDGPU_SVM_RESTORE_WQ_NAME "amdgpu_svm_restore"

static void
amdgpu_svm_range_enqueue(struct amdgpu_svm *svm,
			 struct amdgpu_svm_range *range,
			 unsigned long start,
			 unsigned long last,
			 enum amdgpu_svm_range_queue_op op);

static inline bool
range_has_access(enum amdgpu_svm_attr_access access)
{
	return access == AMDGPU_SVM_ACCESS_ENABLE ||
	       access == AMDGPU_SVM_ACCESS_IN_PLACE;
}

static void
range_invalidate_gpu_mapping(struct drm_gpusvm_range *range)
{
	WRITE_ONCE(to_amdgpu_svm_range(range)->gpu_mapped, false);
}

static bool
range_attr_match(struct drm_gpusvm_range *range,
		 const struct amdgpu_svm_attrs *attrs,
		 uint64_t pte_flags)
{
	struct amdgpu_svm_range *r = to_amdgpu_svm_range(range);

	if (!READ_ONCE(r->gpu_mapped))
		return false;

	return READ_ONCE(r->pte_flags) == pte_flags &&
	       READ_ONCE(r->attr_flags) == attrs->flags;
}

static bool
range_pages_valid(struct amdgpu_svm *svm,
		  struct drm_gpusvm_range *range)
{
	lockdep_assert_held(&svm->gpusvm.notifier_lock);

	if (range->pages.flags.unmapped || range->pages.flags.partial_unmap)
		return false;

	return drm_gpusvm_range_pages_valid(&svm->gpusvm, range);
}


static int
amdgpu_svm_range_gpu_unmap_in_notifier(struct amdgpu_svm *svm,
				      struct drm_gpusvm_range *range,
				      const struct mmu_notifier_range *mmu_range)
{
	struct dma_fence *fence = NULL;
	unsigned long start = max(drm_gpusvm_range_start(range), mmu_range->start);
	unsigned long end = min(drm_gpusvm_range_end(range), mmu_range->end);
	unsigned int flags;
	int ret;

	if (end <= start)
		return 0;

	start >>= PAGE_SHIFT;
	end = (end - 1) >> PAGE_SHIFT;

	flags = memalloc_noreclaim_save();
	ret = amdgpu_vm_update_range(svm->adev, svm->vm, false, true, true, false,
				     NULL, start, end, 0, 0, 0, NULL,
				     NULL, &fence);
	memalloc_noreclaim_restore(flags);

	if (!ret && fence) {
		ret = dma_fence_wait(fence, false);
		if (ret < 0)
			AMDGPU_SVM_TRACE("notifier unmap fence wait failed: ret=%d [0x%lx-0x%lx]-0x%lx\n",
					 ret, start, end,
					 end - start + 1);
	}

	dma_fence_put(fence);
	return ret;
}

static bool
has_always_mapped_range(
			struct drm_gpusvm_notifier *notifier,
			const struct mmu_notifier_range *mmu_range)
{
	struct drm_gpusvm_range *range = NULL;

	drm_gpusvm_for_each_range(range, notifier, mmu_range->start, mmu_range->end) {
		if (READ_ONCE(to_amdgpu_svm_range(range)->attr_flags) &
		    AMDGPU_SVM_FLAG_GPU_ALWAYS_MAPPED)
			return true;
	}

	return false;
}

static uint64_t
amdgpu_svm_range_attr_pte_flags(struct amdgpu_svm *svm,
			    const struct amdgpu_svm_attrs *attrs)
{
	/* WA/POC: a simple pte flags func */
	uint32_t gc_ip_version = amdgpu_ip_version(svm->adev, GC_HWIP, 0);
	uint32_t flags = attrs->flags;
	uint32_t mapping_flags = 0;
	uint64_t pte_flags;
	bool coherent = flags & (AMDGPU_SVM_FLAG_COHERENT |
				 AMDGPU_SVM_FLAG_EXT_COHERENT);
	bool ext_coherent = flags & AMDGPU_SVM_FLAG_EXT_COHERENT;
	bool snoop = true;
	unsigned int mtype_local;

	switch (gc_ip_version) {
	case IP_VERSION(9, 4, 1):
	case IP_VERSION(9, 4, 2):
		mapping_flags |= coherent ?
			AMDGPU_VM_MTYPE_UC : AMDGPU_VM_MTYPE_NC;
		break;
	case IP_VERSION(9, 4, 3):
	case IP_VERSION(9, 4, 4):
	case IP_VERSION(9, 5, 0):
		if (ext_coherent)
			mtype_local = AMDGPU_VM_MTYPE_CC;
		else
			mtype_local = amdgpu_mtype_local == 1 ? AMDGPU_VM_MTYPE_NC :
				amdgpu_mtype_local == 2 ? AMDGPU_VM_MTYPE_CC :
				AMDGPU_VM_MTYPE_RW;
		if (svm->adev->flags & AMD_IS_APU) {
			if (num_possible_nodes() <= 1)
				mapping_flags |= mtype_local;
			else
				mapping_flags |= ext_coherent ?
					AMDGPU_VM_MTYPE_UC : AMDGPU_VM_MTYPE_NC;
		} else {
			if (gc_ip_version < IP_VERSION(9, 5, 0) || ext_coherent)
				mapping_flags |= AMDGPU_VM_MTYPE_UC;
			else
				mapping_flags |= AMDGPU_VM_MTYPE_NC;
		}
		break;
	case IP_VERSION(11, 0, 0):
	case IP_VERSION(11, 0, 1):
	case IP_VERSION(11, 0, 2):
	case IP_VERSION(11, 0, 3):
	case IP_VERSION(11, 0, 4):
	case IP_VERSION(11, 5, 0):
	case IP_VERSION(11, 5, 1):
	case IP_VERSION(11, 5, 2):
	case IP_VERSION(11, 5, 3):
		mapping_flags |= coherent ?
			AMDGPU_VM_MTYPE_UC : AMDGPU_VM_MTYPE_NC;
		break;
	case IP_VERSION(12, 0, 0):
	case IP_VERSION(12, 0, 1):
		mapping_flags |= AMDGPU_VM_MTYPE_NC;
		break;
	default:
		mapping_flags |= coherent ?
			AMDGPU_VM_MTYPE_UC : AMDGPU_VM_MTYPE_NC;
		break;
	}

	if (flags & AMDGPU_SVM_FLAG_GPU_EXEC)
		mapping_flags |= AMDGPU_VM_PAGE_EXECUTABLE;

	pte_flags = AMDGPU_PTE_VALID | AMDGPU_PTE_SYSTEM;
	pte_flags |= snoop ? AMDGPU_PTE_SNOOPED : 0;
	if (gc_ip_version >= IP_VERSION(12, 0, 0))
		pte_flags |= AMDGPU_PTE_IS_PTE;

	amdgpu_gmc_get_vm_pte(svm->adev, svm->vm, NULL, mapping_flags, &pte_flags);
	pte_flags |= AMDGPU_PTE_READABLE;
	if (!(flags & AMDGPU_SVM_FLAG_GPU_RO))
		pte_flags |= AMDGPU_PTE_WRITEABLE;

	return pte_flags;
}

	/*
	* POC/WA: reuse kfd apis for queue quiesce/resume
	* But kfd apis are for process level, not for GPU VM level
	* need consider potential issues
	*/
void amdgpu_svm_range_restore_begin_compute(struct amdgpu_svm *svm)
{
	int ret;

	if (!svm->gpusvm.mm)
		return;

	if (atomic_cmpxchg(&svm->kfd_queues_quiesced, 0, 1) != 0)
		return;

	ret = kgd2kfd_quiesce_mm(svm->gpusvm.mm, KFD_QUEUE_EVICTION_TRIGGER_SVM);
	if (ret == -ESRCH) {
		AMDGPU_SVM_TRACE("kfd quiesce skipped no KFD process\n");
		atomic_set(&svm->kfd_queues_quiesced, 0);
		return;
	}

	if (ret) {
		AMDGPU_SVM_TRACE("kfd quiesce failed ret=%d\n", ret);
		atomic_set(&svm->kfd_queues_quiesced, 0);
		return;
	}

	AMDGPU_SVM_TRACE("kfd quiesce ret=%d\n", ret);
}

void amdgpu_svm_range_restore_end_compute(struct amdgpu_svm *svm)
{
	int ret;

	if (atomic_cmpxchg(&svm->kfd_queues_quiesced, 1, 0) != 1)
		return;

	if (!svm->gpusvm.mm)
		return;

	ret = kgd2kfd_resume_mm(svm->gpusvm.mm);
	if (ret == -ESRCH) {
		AMDGPU_SVM_TRACE("kfd resume skipped no KFD process\n");
		return;
	}

	if (ret)
		AMDGPU_SVM_TRACE("kfd resume failed ret=%d\n", ret);
	else
		AMDGPU_SVM_TRACE("kfd resume ret=%d\n", ret);
}

static int amdgpu_svm_range_lock_vm_pd(struct amdgpu_svm *svm, struct drm_exec *exec)
{
	int ret;

	drm_exec_init(exec, DRM_EXEC_IGNORE_DUPLICATES, 0);
	drm_exec_until_all_locked(exec) {
		ret = amdgpu_vm_lock_pd(svm->vm, exec, 1);
		drm_exec_retry_on_contention(exec);
		if (ret) {
			drm_exec_fini(exec);
			return ret;
		}
	}

	return 0;
}

static int
amdgpu_svm_range_update_gpu(struct amdgpu_svm *svm, unsigned long start_page,
			   unsigned long last_page, uint64_t pte_flags,
			   dma_addr_t *pages_addr, bool flush_tlb,
			   bool update_pdes, bool wait_fence)
{
	struct drm_exec exec;
	struct dma_fence *fence = NULL;
	int ret;

	ret = amdgpu_svm_range_lock_vm_pd(svm, &exec);
	if (ret)
		return ret;

	ret = amdgpu_vm_update_range(svm->adev, svm->vm, false, false,
				     flush_tlb, true,
				     NULL, start_page, last_page, pte_flags, 0, 0,
				     NULL, pages_addr, wait_fence ? &fence : NULL);
	if (!ret && wait_fence && fence) {
		ret = dma_fence_wait(fence, false);
		if (ret < 0)
			AMDGPU_SVM_TRACE("wait unmap fence failed: ret=%d [0x%lx-0x%lx]-0x%lx\n",
					 ret, start_page, last_page,
					 last_page - start_page + 1);
	}
	if (!ret && update_pdes)
		ret = amdgpu_vm_update_pdes(svm->adev, svm->vm, false);

	dma_fence_put(fence);
	drm_exec_fini(&exec);
	return ret;
}

static int
amdgpu_svm_range_update_gpu_range(struct amdgpu_svm *svm,
				  struct drm_gpusvm_range *range,
				  uint64_t pte_flags,
				  bool flush_tlb,
				  bool wait_fence,
				  struct dma_fence **fence)
{
	lockdep_assert_held(&svm->gpusvm.notifier_lock);

	const unsigned long range_start_page = drm_gpusvm_range_start(range) >> PAGE_SHIFT;
	const unsigned long range_end_page = drm_gpusvm_range_end(range) >> PAGE_SHIFT;
	const unsigned long npages = range_end_page - range_start_page;
	unsigned long mapped_pages = 0;
	unsigned long dma_idx = 0;
	int ret;

	if (!range->pages.dma_addr || !npages)
		return -EINVAL;

	while (mapped_pages < npages) {
		const struct drm_pagemap_addr *entry = &range->pages.dma_addr[dma_idx++];
		unsigned long seg_pages = min_t(unsigned long, 1UL << entry->order,
						npages - mapped_pages);
		dma_addr_t seg_addr = entry->addr;
		unsigned long start_page, last_page;
		bool is_last_seg;

		if (entry->proto != DRM_INTERCONNECT_SYSTEM)
			return -EOPNOTSUPP;

		while (mapped_pages + seg_pages < npages) {
			const struct drm_pagemap_addr *next = &range->pages.dma_addr[dma_idx];
			unsigned long next_pages = min_t(unsigned long,
							 1UL << next->order,
							 npages - (mapped_pages + seg_pages));

			if (next->proto != entry->proto ||
			    next->addr != seg_addr + ((dma_addr_t)seg_pages << PAGE_SHIFT))
				break;

			seg_pages += next_pages;
			dma_idx++;
		}

		start_page = range_start_page + mapped_pages;
		last_page = start_page + seg_pages - 1;
		is_last_seg = mapped_pages + seg_pages == npages;

		ret = amdgpu_vm_update_range(svm->adev, svm->vm, false, false,
					     flush_tlb && is_last_seg, true, NULL,
					     start_page, last_page, pte_flags,
					     0, seg_addr, NULL, NULL,
					     wait_fence && is_last_seg ? fence : NULL);
		if (ret)
			return ret;

		mapped_pages += seg_pages;
	}

	return 0;
}

static int
amdgpu_svm_range_map(struct amdgpu_svm *svm,
		       unsigned long start,
		       unsigned long end,
		       const struct amdgpu_svm_attrs *attrs,
		       const struct drm_gpusvm_ctx *gpusvm_ctx,
		       uint64_t pte_flags)
{
	unsigned long addr = start;
	int ret;

	while (addr < end) {
		struct drm_exec exec;
		struct drm_gpusvm_ctx map_ctx;
		struct drm_gpusvm_range *range;
		struct dma_fence *fence = NULL;
		unsigned long vma_start;
		unsigned long next_addr;
		uint64_t range_pte_flags;
		unsigned int flags;
		bool skip_map;

		vma_start = drm_gpusvm_find_vma_start(&svm->gpusvm, addr, end);
		if (vma_start > addr)
			return -EFAULT;

		map_ctx = *gpusvm_ctx;
retry:
		range = drm_gpusvm_range_find_or_insert(&svm->gpusvm, addr,
							vma_start, end,
							&map_ctx);
		if (IS_ERR(range)) {
			ret = PTR_ERR(range);
			/*
			 * drm gpu svm deny RO when VMA is writeable
			 * but some UMD test does not set RO in readonly MM VMA
			 * so set read only when ret == -EPERM and retry
			 */
			if (ret == -EPERM && !map_ctx.read_only) {
				map_ctx.read_only = true;
				goto retry;
			}
			return ret;
		}

		next_addr = drm_gpusvm_range_end(range);
		if (next_addr <= addr)
			return -EINVAL;

		range_pte_flags = map_ctx.read_only ?
			(pte_flags & ~AMDGPU_PTE_WRITEABLE) : pte_flags;

		skip_map = range_attr_match(range, attrs, range_pte_flags);

		AMDGPU_SVM_TRACE("range_map: [0x%lx-0x%lx] skip=%d pte=0x%llx\n",
				 addr, next_addr, skip_map ? 1 : 0, range_pte_flags);

		if (!skip_map) {
			ret = drm_gpusvm_range_get_pages(&svm->gpusvm, range, &map_ctx);
			if (ret)
				return ret;
		}

		ret = amdgpu_svm_range_lock_vm_pd(svm, &exec);
		if (ret)
			return ret;

		flags = memalloc_noreclaim_save();
		drm_gpusvm_notifier_lock(&svm->gpusvm);
		if (skip_map) {
			/* slow path must validate under notifier lock */
			if (!range_attr_match(range, attrs, range_pte_flags) ||
			    !range_pages_valid(svm, range)) {
				range_invalidate_gpu_mapping(range);
				ret = -EAGAIN;
			} else {
				ret = 0;
			}
		} else if (!range_pages_valid(svm, range)) {
			/* not protected by mmap lock, maybe changed by mmu notifier */
			ret = -EAGAIN;
		} else {
			ret = amdgpu_svm_range_update_gpu_range(svm, range,
								range_pte_flags,
								true, true, &fence);
		}
		drm_gpusvm_notifier_unlock(&svm->gpusvm);
		memalloc_noreclaim_restore(flags);

		if (!ret && fence)
			dma_fence_wait(fence, false);

		dma_fence_put(fence);

		if (!ret)
			ret = amdgpu_vm_update_pdes(svm->adev, svm->vm, false);
		if (!ret) {
			svm->flush_tlb(svm);
			WRITE_ONCE(to_amdgpu_svm_range(range)->pte_flags, range_pte_flags);
			WRITE_ONCE(to_amdgpu_svm_range(range)->attr_flags, attrs->flags);
			WRITE_ONCE(to_amdgpu_svm_range(range)->gpu_mapped, true);
		}
		drm_exec_fini(&exec);

		if (ret)
			return ret;

		addr = next_addr;
	}

	return 0;
}

static int
amdgpu_svm_range_map_interval(struct amdgpu_svm *svm, unsigned long start_page,
				unsigned long last_page,
				const struct amdgpu_svm_attrs *attrs)
{
	struct drm_gpusvm_ctx gpusvm_ctx = {
		.read_only = !!(attrs->flags & AMDGPU_SVM_FLAG_GPU_RO),
	};
	unsigned long start = start_page << PAGE_SHIFT;
	unsigned long end = (last_page + 1) << PAGE_SHIFT;
	uint64_t pte_flags;
	int ret;

	pte_flags = amdgpu_svm_range_attr_pte_flags(svm, attrs);

	ret = amdgpu_svm_range_map(svm, start, end, attrs, &gpusvm_ctx,
				   pte_flags);
	if (ret)
		AMDGPU_SVM_TRACE("map_interval failed: ret=%d [0x%lx-0x%lx)-0x%lx\n",
				 ret, start, end, end - start);

	return ret;
}

int
amdgpu_svm_range_map_attr_ranges(struct amdgpu_svm *svm,
				 unsigned long start_page,
				 unsigned long last_page)
{
	lockdep_assert_held_write(&svm->svm_lock);

	struct amdgpu_svm_attr_tree *attr_tree = svm->attr_tree;
	unsigned long cursor = start_page;

	while (cursor <= last_page) {
		struct amdgpu_svm_attrs attrs;
		unsigned long seg_last;
		unsigned long next;
		int ret;

		mutex_lock(&attr_tree->lock);
		amdgpu_svm_attr_lookup_page_locked(attr_tree, cursor, &attrs,
						   &seg_last);
		mutex_unlock(&attr_tree->lock);

		seg_last = min(seg_last, last_page);
		if (range_has_access(attrs.access)) {
			/* map may fail here cause no vma or access deny */
			ret = amdgpu_svm_range_map_interval(svm, cursor, seg_last,
							    &attrs);
			if (ret)
				return ret;
		}

		if (seg_last == ULONG_MAX || seg_last == last_page)
			break;

		next = seg_last + 1;
		if (next <= cursor)
			break;
		cursor = next;
	}

	return 0;
}

static void amdgpu_svm_range_remove(struct amdgpu_svm *svm,
						   struct drm_gpusvm_range *range,
						   struct drm_gpusvm_ctx *ctx)
{
	lockdep_assert_held_write(&svm->svm_lock);

	if (!range->pages.flags.unmapped && !range->pages.flags.partial_unmap)
		drm_gpusvm_range_unmap_pages(&svm->gpusvm, range, ctx);

	range_invalidate_gpu_mapping(range);
	drm_gpusvm_range_remove(&svm->gpusvm, range);
}

static bool
amdgpu_svm_range_remove_overlaps(struct amdgpu_svm *svm, unsigned long start_page,
				      unsigned long last_page,
				      unsigned long *rebuild_start,
				      unsigned long *rebuild_last)
{
	lockdep_assert_held_write(&svm->svm_lock);

	struct drm_gpusvm_ctx ctx = {
		.in_notifier = false,
	};
	unsigned long start = start_page << PAGE_SHIFT;
	unsigned long end = (last_page + 1) << PAGE_SHIFT;
	struct drm_gpusvm_notifier *notifier, *next_notifier;
	bool removed = false;

	if (rebuild_start && rebuild_last) {
		*rebuild_start = ULONG_MAX;
		*rebuild_last = 0;
	}

	/* remove overlap ranges, need to remove entire range */
	drm_gpusvm_for_each_notifier_safe(notifier, next_notifier, &svm->gpusvm,
					  start, end) {
		struct drm_gpusvm_range *range, *next_range;

		drm_gpusvm_for_each_range_safe(range, next_range, notifier, start,
					       end) {
			unsigned long rs = drm_gpusvm_range_start(range) >> PAGE_SHIFT;
			unsigned long rl = (drm_gpusvm_range_end(range) >> PAGE_SHIFT) - 1;

			removed = true;
			/* record rebuild start end, first range start and last range end */
			if (rebuild_start && rebuild_last) {
				*rebuild_start = min(*rebuild_start, rs);
				*rebuild_last = max(*rebuild_last, rl);
			}
			amdgpu_svm_range_remove(svm, range, &ctx);
		}
	}

	return removed;
}

static int amdgpu_svm_range_rebuild_locked(struct amdgpu_svm *svm,
				  unsigned long start_page,
				  unsigned long last_page,
				  bool rebuild)
{
	unsigned long rebuild_start = start_page;
	unsigned long rebuild_last = last_page;
	bool removed;
	int ret;

	lockdep_assert_held_write(&svm->svm_lock);

	AMDGPU_SVM_TRACE("remove and rebuild: [0x%lx-0x%lx] rebuild=%d\n",
			 start_page, last_page, rebuild ? 1 : 0);

	removed = amdgpu_svm_range_remove_overlaps(svm, start_page, last_page,
						   &rebuild_start,
						   &rebuild_last);
	if (!removed)
		return 0;

	/* scan rebuild start end to build the extra removed ranges */
	if (rebuild)
		return amdgpu_svm_range_map_attr_ranges(svm, rebuild_start,
							rebuild_last);

	ret = amdgpu_svm_range_update_gpu(svm, rebuild_start, rebuild_last,
					  0, NULL, true, true, true);
	if (!ret)
		svm->flush_tlb(svm);

	return ret;
}

static void
amdgpu_svm_range_process_notifier_ranges(struct amdgpu_svm *svm,
					 struct drm_gpusvm_notifier *notifier,
					 const struct mmu_notifier_range *mmu_range,
					 uint32_t notifier_op,
					 enum amdgpu_svm_range_queue_op queue_op)
{
	struct drm_gpusvm_ctx ctx = {
		.in_notifier = true,
	};
	struct drm_gpusvm_range *range = NULL;
	bool queue_ranges = notifier_op & AMDGPU_SVM_RANGE_NOTIFIER_QUEUE_INTERVAL;
	bool clear_pte = notifier_op & AMDGPU_SVM_RANGE_NOTIFIER_CLEAR_PTE;
	bool is_unmap = mmu_range->event == MMU_NOTIFY_UNMAP;
	bool has_range = false;

	lockdep_assert_held(&svm->gpusvm.notifier_lock);

	drm_gpusvm_for_each_range(range, notifier, mmu_range->start, mmu_range->end) {
		has_range = true;
		if (clear_pte) {
			amdgpu_svm_range_gpu_unmap_in_notifier(svm, range,
									   mmu_range);
			range_invalidate_gpu_mapping(range);
		}

		drm_gpusvm_range_unmap_pages(&svm->gpusvm, range, &ctx);
		if (is_unmap)
			drm_gpusvm_range_set_unmapped(range, mmu_range);

		if (queue_ranges) {
			unsigned long start = max(drm_gpusvm_range_start(range),
						  mmu_range->start) >> PAGE_SHIFT;
			unsigned long last = (min(drm_gpusvm_range_end(range),
						  mmu_range->end) - 1) >> PAGE_SHIFT;

			amdgpu_svm_range_enqueue(svm, to_amdgpu_svm_range(range),
						 start, last, queue_op);
		}
	}

	if (has_range && clear_pte)
		svm->flush_tlb(svm);
}

static bool
amdgpu_svm_range_interval_has_range(struct amdgpu_svm *svm,
					     unsigned long start_page,
					     unsigned long last_page)
{
	lockdep_assert_held(&svm->svm_lock);

	unsigned long start = start_page << PAGE_SHIFT;
	unsigned long end = (last_page + 1) << PAGE_SHIFT;
	struct drm_gpusvm_notifier *notifier;

	drm_gpusvm_for_each_notifier(notifier, &svm->gpusvm, start, end) {
		struct drm_gpusvm_range *range = NULL;

		drm_gpusvm_for_each_range(range, notifier, start, end)
			return true;
	}

	return false;
}

int amdgpu_svm_range_apply_attr_change(struct amdgpu_svm *svm,
				       unsigned long start,
				       unsigned long last,
				       uint32_t trigger,
				       const struct amdgpu_svm_attrs *prev_attrs,
				       const struct amdgpu_svm_attrs *new_attrs)
{
	lockdep_assert_held_write(&svm->svm_lock);

	bool old_access, new_access;
	bool update_mapping = false;

	old_access = range_has_access(prev_attrs->access);
	new_access = range_has_access(new_attrs->access);

	AMDGPU_SVM_TRACE("attr change trigger=0x%x old_access=%d new_access=%d [0x%lx-0x%lx]-0x%lx, xnack=%d\n",
			 trigger, old_access, new_access, start, last, last - start + 1,
			 svm->xnack_enabled ? 1 : 0);

	if (trigger & AMDGPU_SVM_ATTR_TRIGGER_ACCESS_CHANGE) {
		if (!new_access && old_access) {
			/*
			 * Do nothing align with kfd svm
			 * TODO: unmap ranges from GPU that lost access
			 */
			AMDGPU_SVM_TRACE("skip unmap ioctl operation [0x%lx-0x%lx]-0x%lx\n",
					 start, last, last - start + 1);
		} else if (new_access) {
			if (NEED_REBUILD(svm) ||
			    (new_attrs->flags & AMDGPU_SVM_FLAG_GPU_ALWAYS_MAPPED))
				update_mapping = true;
		}
	}

	if ((trigger & (AMDGPU_SVM_ATTR_TRIGGER_PTE_FLAG_CHANGE |
			AMDGPU_SVM_ATTR_TRIGGER_MAPPING_FLAG_CHANGE)) &&
	    new_access)
		update_mapping = true;

	if (trigger & AMDGPU_SVM_ATTR_TRIGGER_LOCATION_CHANGE) {
		/* TODO: add migration */
	}

	if (!update_mapping)
		return 0;

	AMDGPU_SVM_TRACE("mapping update: remap interval [0x%lx-0x%lx]-0x%lx\n",
			 start, last, last - start + 1);
	return amdgpu_svm_range_map_interval(svm, start, last, new_attrs);
}

static bool
range_dequeue_locked(struct amdgpu_svm *svm,
					struct list_head *work_list,
					bool restore_queue,
					struct range_pending_op_ctx *op_ctx)
{
	struct amdgpu_svm_range *range;

	lockdep_assert_held(&svm->gc_lock);

	range = list_first_entry_or_null(work_list, struct amdgpu_svm_range,
					 gc_node);
	if (!range)
		return false;

	list_del_init(&range->gc_node);
	if (restore_queue)
		range->restore_queued = false;
	else
		range->gc_queued = false;

	op_ctx->range = range;
	op_ctx->start = range->pending_start;
	op_ctx->last = range->pending_last;
	op_ctx->pending_ops = range->pending_ops;

	range->pending_start = ULONG_MAX;
	range->pending_last = 0;
	range->pending_ops = AMDGPU_SVM_RANGE_PENDING_OP_NONE;

	return true;
}

static void
range_requeue_restore_locked(struct amdgpu_svm *svm,
					struct amdgpu_svm_range *range,
					unsigned long start,
					unsigned long last)
{
	lockdep_assert_held(&svm->gc_lock);

	range->pending_start = min(range->pending_start, start);
	range->pending_last = max(range->pending_last, last);
	range->pending_ops |= AMDGPU_SVM_RANGE_PENDING_OP_RESTORE;

	if (!range->gc_queued && !range->restore_queued) {
		list_add_tail(&range->gc_node, &svm->restore_work_list);
		range->restore_queued = true;
	}
}

static bool
range_try_dequeue(struct amdgpu_svm_range *range)
{
	if (!range->in_queue)
		return false;

	if (range->gc_queued || range->restore_queued ||
	    range->pending_start <= range->pending_last ||
	    range->pending_ops != AMDGPU_SVM_RANGE_PENDING_OP_NONE)
		return false;

	range->in_queue = false;
	return true;
}

static void
range_put_if_dequeued(struct amdgpu_svm *svm,
				     struct amdgpu_svm_range *range)
{
	bool dequeue;

	spin_lock(&svm->gc_lock);
	dequeue = range_try_dequeue(range);
	spin_unlock(&svm->gc_lock);

	if (dequeue)
		drm_gpusvm_range_put(&range->base);
}

static void
amdgpu_svm_range_enqueue(struct amdgpu_svm *svm,
			 struct amdgpu_svm_range *range,
			 unsigned long start,
			 unsigned long last,
			 enum amdgpu_svm_range_queue_op op)
{
	bool queue_gc_work = false;
	bool queue_restore_work = false;

	if (atomic_read(&svm->exiting))
		return;

	spin_lock(&svm->gc_lock);
	if (!range->in_queue) {
		drm_gpusvm_range_get(&range->base);
		range->in_queue = true;
	}

	range->pending_start = min(range->pending_start, start);
	range->pending_last = max(range->pending_last, last);

	switch (op) {
	case AMDGPU_SVM_RANGE_OP_UNMAP:
		range->pending_ops |= AMDGPU_SVM_RANGE_PENDING_OP_UNMAP;
		if (NEED_REBUILD(svm))
			range->pending_ops |= AMDGPU_SVM_RANGE_PENDING_OP_RESTORE;
		break;
	case AMDGPU_SVM_RANGE_OP_RESTORE:
		range->pending_ops |= AMDGPU_SVM_RANGE_PENDING_OP_RESTORE;
		break;
	}

	if (UNMAP_WORK(range->pending_ops)) {
		if (range->restore_queued) {
			list_move_tail(&range->gc_node, &svm->gc_list);
			range->restore_queued = false;
			range->gc_queued = true;
		} else if (!range->gc_queued) {
			list_add_tail(&range->gc_node, &svm->gc_list);
			range->gc_queued = true;
		}
		queue_gc_work = true;
	} else if (RESTORE_WORK(range->pending_ops)) {
		if (!range->gc_queued && !range->restore_queued) {
			list_add_tail(&range->gc_node, &svm->restore_work_list);
			range->restore_queued = true;
		}
		queue_restore_work = true;
	}

	spin_unlock(&svm->gc_lock);

	if (queue_gc_work)
		queue_work(svm->gc_wq, &svm->gc_work);
	if (queue_restore_work)
		queue_delayed_work(svm->restore_wq, &svm->restore_work,
				   msecs_to_jiffies(AMDGPU_SVM_RANGE_RESTORE_DELAY_MS));
}

static int
amdgpu_svm_range_process_unmap_interval(struct amdgpu_svm *svm,
					  unsigned long start, unsigned long last,
					  bool rebuild)
{
	int ret = 0;

	down_write(&svm->svm_lock);
	/* clean attrs */
	amdgpu_svm_attr_clear_pages(svm->attr_tree, start, last);

	/* rebuild if needed */
	if (amdgpu_svm_range_interval_has_range(svm, start, last))
		ret = amdgpu_svm_range_rebuild_locked(svm, start, last, rebuild);

	up_write(&svm->svm_lock);

	AMDGPU_SVM_TRACE("work=UNMAP ret=%d start=0x%lx last=0x%lx rebuild=%d\n",
		ret, start, last, rebuild ? 1 : 0);

	return ret;
}

static void amdgpu_svm_range_begin_restore(struct amdgpu_svm *svm)
{
	if (atomic_inc_return(&svm->evicted_ranges) != 1)
		return;

	svm->begin_restore(svm);
}

static void amdgpu_svm_range_restore_worker(struct work_struct *w)
{
	struct delayed_work *dwork = to_delayed_work(w);
	struct amdgpu_svm *svm = container_of(dwork, struct amdgpu_svm, restore_work);
	unsigned long resched_delay =
		max_t(unsigned long, 1,
		      msecs_to_jiffies(AMDGPU_SVM_RANGE_RESTORE_DELAY_MS));
	struct range_pending_op_ctx op_ctx;
	int evicted_record;
	bool need_resched = false;
	bool has_pending;
	int ret;

	if (atomic_read(&svm->exiting))
		return;

	evicted_record = atomic_read(&svm->evicted_ranges);
	if (!evicted_record)
		return;

	if (!svm->gpusvm.mm) {
		atomic_set(&svm->evicted_ranges, 0);
		svm->end_restore(svm);
		return;
	}

	spin_lock(&svm->gc_lock);
	while (range_dequeue_locked(svm, &svm->restore_work_list,
				    true, &op_ctx)) {
		spin_unlock(&svm->gc_lock);

		down_write(&svm->svm_lock);
		ret = amdgpu_svm_range_map_attr_ranges(svm, op_ctx.start,
						       op_ctx.last);
		up_write(&svm->svm_lock);

		if (ret) {
			AMDGPU_SVM_TRACE("restore work retry ret=%d start=0x%lx last=0x%lx ret=%d\n",
					 ret, op_ctx.start, op_ctx.last, ret);
			spin_lock(&svm->gc_lock);
			range_requeue_restore_locked(svm, op_ctx.range,
								op_ctx.start, op_ctx.last);
			spin_unlock(&svm->gc_lock);
			need_resched = true;
		}

		range_put_if_dequeued(svm, op_ctx.range);
		spin_lock(&svm->gc_lock);
	}
	spin_unlock(&svm->gc_lock);

	spin_lock(&svm->gc_lock);
	has_pending = !list_empty(&svm->restore_work_list) ||
		      !list_empty(&svm->gc_list);
	spin_unlock(&svm->gc_lock);

	if (!need_resched && !has_pending) {

		drm_gpusvm_notifier_lock(&svm->gpusvm);
		spin_lock(&svm->gc_lock);

		has_pending = !list_empty(&svm->restore_work_list) || !list_empty(&svm->gc_list);

		spin_unlock(&svm->gc_lock);

		if (!has_pending &&
			atomic_cmpxchg(&svm->evicted_ranges, evicted_record, 0) == evicted_record) {

			drm_gpusvm_notifier_unlock(&svm->gpusvm);
			svm->end_restore(svm);
			return;
	
		}
		drm_gpusvm_notifier_unlock(&svm->gpusvm);
	}

	queue_delayed_work(svm->restore_wq, &svm->restore_work, resched_delay);
}

static void amdgpu_svm_range_gc_worker(struct work_struct *w)
{
	struct amdgpu_svm *svm = container_of(w, struct amdgpu_svm, gc_work);
	struct range_pending_op_ctx op_ctx;

	spin_lock(&svm->gc_lock);
	while (range_dequeue_locked(svm, &svm->gc_list,
				    false, &op_ctx)) {
		int ret = 0;

		spin_unlock(&svm->gc_lock);

		if (UNMAP_WORK(op_ctx.pending_ops))
			ret = amdgpu_svm_range_process_unmap_interval(svm,
					op_ctx.start, op_ctx.last,
					NEED_REBUILD(svm));

		if (RESTORE_WORK(op_ctx.pending_ops)) {
			/* queue into restore wq, if rebuild failed */
			if (NEED_REBUILD(svm) && !ret)
				queue_delayed_work(svm->restore_wq,
					&svm->restore_work,
					msecs_to_jiffies(AMDGPU_SVM_RANGE_RESTORE_DELAY_MS));
			else
				amdgpu_svm_range_enqueue(svm, op_ctx.range,
							 op_ctx.start,
							 op_ctx.last,
							 AMDGPU_SVM_RANGE_OP_RESTORE);
		}

		range_put_if_dequeued(svm, op_ctx.range);
		spin_lock(&svm->gc_lock);
	}
	spin_unlock(&svm->gc_lock);
}

void amdgpu_svm_range_invalidate(struct amdgpu_svm *svm,
				 struct drm_gpusvm_notifier *notifier,
				 const struct mmu_notifier_range *mmu_range)
{
	bool is_unmap = mmu_range->event == MMU_NOTIFY_UNMAP;
	uint32_t op;
	enum amdgpu_svm_range_queue_op queue_op;

	if (mmu_range->event == MMU_NOTIFY_RELEASE)
		return;
	if (atomic_read(&svm->exiting))
		return;

	if (!drm_gpusvm_range_find(notifier, mmu_range->start,
				    mmu_range->end))
		return;

	if (is_unmap) {
		op = AMDGPU_SVM_RANGE_NOTIFIER_CLEAR_PTE |
			 AMDGPU_SVM_RANGE_NOTIFIER_QUEUE_INTERVAL;
		queue_op = AMDGPU_SVM_RANGE_OP_UNMAP;
		if (NEED_REBUILD(svm))
			amdgpu_svm_range_begin_restore(svm);
	} else if (NEED_REBUILD(svm) ||
		   has_always_mapped_range(notifier, mmu_range)) {
		op = AMDGPU_SVM_RANGE_NOTIFIER_QUEUE_INTERVAL;
		queue_op = AMDGPU_SVM_RANGE_OP_RESTORE;
		amdgpu_svm_range_begin_restore(svm);
	} else {
		op = AMDGPU_SVM_RANGE_NOTIFIER_CLEAR_PTE;
		queue_op = AMDGPU_SVM_RANGE_OP_RESTORE;
	}

	amdgpu_svm_range_process_notifier_ranges(svm, notifier, mmu_range,
						 op, queue_op);
}

int amdgpu_svm_range_work_init(struct amdgpu_svm *svm)
{
	svm->gc_wq = alloc_workqueue(AMDGPU_SVM_RANGE_WQ_NAME,
					WQ_UNBOUND | WQ_HIGHPRI | WQ_MEM_RECLAIM, 0);
	if (!svm->gc_wq)
		return -ENOMEM;

	svm->restore_wq = alloc_ordered_workqueue(AMDGPU_SVM_RESTORE_WQ_NAME,
						  WQ_HIGHPRI | WQ_MEM_RECLAIM);
	if (!svm->restore_wq) {
		destroy_workqueue(svm->gc_wq);
		svm->gc_wq = NULL;
		return -ENOMEM;
	}

	init_rwsem(&svm->svm_lock);
	spin_lock_init(&svm->gc_lock);
	INIT_LIST_HEAD(&svm->gc_list);
	INIT_LIST_HEAD(&svm->restore_work_list);
	INIT_WORK(&svm->gc_work, amdgpu_svm_range_gc_worker);
	INIT_DELAYED_WORK(&svm->restore_work, amdgpu_svm_range_restore_worker);

	return 0;
}

void amdgpu_svm_range_flush(struct amdgpu_svm *svm)
{
	flush_work(&svm->gc_work);
	flush_delayed_work(&svm->restore_work);
	flush_work(&svm->gc_work);
}

void amdgpu_svm_range_sync_work(struct amdgpu_svm *svm)
{
	amdgpu_svm_range_flush(svm);
	flush_workqueue(svm->gc_wq);
	flush_workqueue(svm->restore_wq);
}

static void
amdgpu_svm_range_clean_queue(struct amdgpu_svm *svm,
			     struct list_head *work_list,
			     bool restore_queue)
{
	struct range_pending_op_ctx op_ctx;

	spin_lock(&svm->gc_lock);
	while (range_dequeue_locked(svm, work_list,
				    restore_queue, &op_ctx)) {
		spin_unlock(&svm->gc_lock);
		range_put_if_dequeued(svm, op_ctx.range);
		spin_lock(&svm->gc_lock);
	}
	spin_unlock(&svm->gc_lock);
}

void amdgpu_svm_range_work_fini(struct amdgpu_svm *svm)
{
	cancel_delayed_work_sync(&svm->restore_work);
	flush_work(&svm->gc_work);
	amdgpu_svm_range_clean_queue(svm, &svm->gc_list, false);
	amdgpu_svm_range_clean_queue(svm, &svm->restore_work_list, true);
	atomic_set(&svm->evicted_ranges, 0);
	if (atomic_read(&svm->kfd_queues_quiesced))
		svm->end_restore(svm);

	destroy_workqueue(svm->restore_wq);
	svm->restore_wq = NULL;
	destroy_workqueue(svm->gc_wq);
	svm->gc_wq = NULL;
}
