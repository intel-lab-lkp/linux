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
#include "amdgpu_svm_range.h"
#include "amdgpu_svm_fault.h"
#include "amdgpu.h"
#include "amdgpu_vm.h"

#include <drm/drm_exec.h>
#include <drm/drm_pagemap.h>

#include <linux/mmu_notifier.h>
#include <uapi/linux/kfd_ioctl.h>

bool
amdgpu_svm_range_pages_valid(struct amdgpu_svm *svm,
		  struct amdgpu_svm_range *range)
{
	struct drm_gpusvm_range *base = &range->base;

	lockdep_assert_held(&svm->gpusvm.notifier_lock);

	if (base->pages.flags.unmapped || base->pages.flags.partial_unmap)
		return false;

	return drm_gpusvm_range_pages_valid(&svm->gpusvm, base);
}

bool amdgpu_svm_range_is_valid(struct amdgpu_svm *svm,
			       struct amdgpu_svm_range *range,
			       const struct amdgpu_svm_attrs *attrs)
{
	unsigned int flags;
	bool valid;

	flags = memalloc_noreclaim_save();
	drm_gpusvm_notifier_lock(&svm->gpusvm);
	valid = range->gpu_mapped &&
		range->attr_flags == attrs->flags &&
		amdgpu_svm_range_pages_valid(svm, range);
	drm_gpusvm_notifier_unlock(&svm->gpusvm);
	memalloc_noreclaim_restore(flags);

	return valid;
}


int
amdgpu_svm_range_zap_ptes(struct amdgpu_svm *svm,
			  struct amdgpu_svm_range *range,
			  unsigned long start_page,
			  unsigned long last_page)
{
	struct dma_fence *fence = NULL;
	unsigned int flags;
	int ret;

	if (last_page < start_page)
		return 0;

	flags = memalloc_noreclaim_save();
	ret = amdgpu_vm_update_range(svm->adev, svm->vm, false, true, true, false,
				     NULL, start_page, last_page, 0, 0, 0, NULL,
				     NULL, &fence);
	memalloc_noreclaim_restore(flags);

	if (!ret && fence) {
		ret = dma_fence_wait(fence, false);
		if (ret < 0)
			AMDGPU_SVM_TRACE(
				"notifier unmap fence wait failed: ret=%d [0x%lx-0x%lx]-0x%lx\n",
				ret, start_page, last_page,
				last_page - start_page + 1);
	}

	dma_fence_put(fence);
	return ret;
}

uint64_t
amdgpu_svm_range_attr_pte_flags(struct amdgpu_svm *svm,
			    const struct amdgpu_svm_attrs *attrs,
			    bool read_only,
			    enum drm_interconnect_protocol proto)
{
	uint32_t flags = attrs->flags;
	uint32_t mapping_flags = 0;
	uint32_t gc_ip_version = amdgpu_ip_version(svm->adev, GC_HWIP, 0);
	uint64_t pte_flags;
	bool snoop = proto != AMDGPU_INTERCONNECT_VRAM;
	bool coherent = flags & (AMDGPU_SVM_ATTR_BIT_COHERENT |
				 AMDGPU_SVM_ATTR_BIT_EXT_COHERENT);
	bool ext_coherent = flags & AMDGPU_SVM_ATTR_BIT_EXT_COHERENT;
	unsigned int mtype_local, mtype_remote;
	bool is_aid_a1;
	bool is_local = (proto == AMDGPU_INTERCONNECT_VRAM);
	bool is_vram = is_local || (proto == AMDGPU_INTERCONNECT_P2P);

	switch (gc_ip_version) {
	case IP_VERSION(9, 4, 1):
	case IP_VERSION(9, 4, 2):
		if (is_local) {
			mapping_flags |= coherent ?
				AMDGPU_VM_MTYPE_CC : AMDGPU_VM_MTYPE_RW;
			/* 9.4.2 local VRAM with XGMI keeps snoop */
			if (gc_ip_version == IP_VERSION(9, 4, 2) &&
			    svm->adev->gmc.xgmi.connected_to_cpu)
				snoop = true;
		} else {
			mapping_flags |= coherent ?
				AMDGPU_VM_MTYPE_UC : AMDGPU_VM_MTYPE_NC;
			/* TODO: migration: re enable snoop for same hive */
		}
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
		snoop = true;
		if (is_vram) {
			if (is_local) {
				/* local HBM region close to partition */
				mapping_flags |= mtype_local;
			} else if (!ext_coherent) {
				/* TODO: add same hive check */
				mapping_flags |= AMDGPU_VM_MTYPE_NC;
			} else if (gc_ip_version < IP_VERSION(9, 5, 0)) {
				/* TODO: add same hive check */
				mapping_flags |= AMDGPU_VM_MTYPE_UC;
			} else {
				mapping_flags |= ext_coherent ?
					AMDGPU_VM_MTYPE_UC : AMDGPU_VM_MTYPE_NC;
			}
		} else if (svm->adev->flags & AMD_IS_APU) {
			/* On NUMA systems, locality is determined per-page
			 * in amdgpu_gmc_override_vm_pte_flags.
			 */
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
	case IP_VERSION(12, 0, 0):
	case IP_VERSION(12, 0, 1):
		mapping_flags |= AMDGPU_VM_MTYPE_NC;
		break;
	case IP_VERSION(12, 1, 0):
		is_aid_a1 = (svm->adev->rev_id & 0x10);
		mtype_local = amdgpu_mtype_local == 0 ? AMDGPU_VM_MTYPE_RW :
				amdgpu_mtype_local == 1 ? AMDGPU_VM_MTYPE_NC :
				is_aid_a1 ? AMDGPU_VM_MTYPE_RW : AMDGPU_VM_MTYPE_NC;
		mtype_remote = is_aid_a1 ? AMDGPU_VM_MTYPE_NC : AMDGPU_VM_MTYPE_UC;
		snoop = true;

		if (is_local) {
			mapping_flags |= mtype_local;
		} else if (ext_coherent) {
			mapping_flags |= AMDGPU_VM_MTYPE_UC;
		} else {
			/* system memory or remote VRAM */
			mapping_flags |= mtype_remote;
		}
		break;
	default:
		mapping_flags |= coherent ?
			AMDGPU_VM_MTYPE_UC : AMDGPU_VM_MTYPE_NC;
		break;
	}

	if (flags & AMDGPU_SVM_ATTR_BIT_GPU_EXEC)
		mapping_flags |= AMDGPU_VM_PAGE_EXECUTABLE;

	pte_flags = AMDGPU_PTE_VALID;
	pte_flags |= is_local ? 0 : AMDGPU_PTE_SYSTEM;
	pte_flags |= snoop ? AMDGPU_PTE_SNOOPED : 0;
	if (gc_ip_version >= IP_VERSION(12, 0, 0))
		pte_flags |= AMDGPU_PTE_IS_PTE;

	amdgpu_gmc_get_vm_pte(svm->adev, svm->vm, NULL, mapping_flags, &pte_flags);
	pte_flags |= AMDGPU_PTE_READABLE;
	if (!(flags & AMDGPU_SVM_ATTR_BIT_GPU_RO) && !read_only)
		pte_flags |= AMDGPU_PTE_WRITEABLE;

	if (gc_ip_version == IP_VERSION(12, 1, 0) &&
	    svm->adev->have_atomics_support)
		pte_flags |= AMDGPU_PTE_BUS_ATOMICS;

	return pte_flags;
}



int amdgpu_svm_range_lock_vm_pd(struct amdgpu_svm *svm, struct drm_exec *exec,
				bool intr)
{
	unsigned int exec_flags = DRM_EXEC_IGNORE_DUPLICATES;
	int ret;

	if (intr)
		exec_flags |= DRM_EXEC_INTERRUPTIBLE_WAIT;

	drm_exec_init(exec, exec_flags, 0);
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

int
amdgpu_svm_range_update_gpu_range(struct amdgpu_svm *svm,
				  struct amdgpu_svm_range *range,
				  const struct amdgpu_svm_attrs *attrs,
				  bool read_only,
				  bool flush_tlb,
				  bool wait_fence,
				  struct dma_fence **fence)
{
	struct drm_gpusvm_range *base = &range->base;

	lockdep_assert_held(&svm->gpusvm.notifier_lock);

	const unsigned long range_start_page = drm_gpusvm_range_start(base) >> PAGE_SHIFT;
	const unsigned long range_end_page = drm_gpusvm_range_end(base) >> PAGE_SHIFT;
	const unsigned long npages = range_end_page - range_start_page;
	unsigned long mapped_pages = 0;
	unsigned long dma_idx = 0;
	int ret;

	if (!base->pages.dma_addr || !npages)
		return -EINVAL;

	while (mapped_pages < npages) {
		const struct drm_pagemap_addr *entry = &base->pages.dma_addr[dma_idx++];
		unsigned long seg_pages = min_t(unsigned long, 1UL << entry->order,
						npages - mapped_pages);
		uint64_t pte_flags;
		unsigned long start_page, last_page;
		bool is_last_seg;

		if (entry->proto != DRM_INTERCONNECT_SYSTEM)
			return -EOPNOTSUPP;

		pte_flags = amdgpu_svm_range_attr_pte_flags(svm, attrs,
							    read_only,
							    entry->proto);

		start_page = range_start_page + mapped_pages;
		last_page = start_page + seg_pages - 1;
		mapped_pages += seg_pages;
		is_last_seg = mapped_pages == npages;

		ret = amdgpu_vm_update_range(svm->adev, svm->vm, false, false,
					     flush_tlb && is_last_seg, true, NULL,
					     start_page, last_page, pte_flags,
					     0, entry->addr, NULL, NULL,
					     wait_fence && is_last_seg ? fence : NULL);
		if (ret)
			return ret;
	}

	return 0;
}

struct amdgpu_svm_range *
amdgpu_svm_range_find_or_insert(struct amdgpu_svm *svm, unsigned long addr,
				unsigned long gpuva_start, unsigned long gpuva_end,
				struct drm_gpusvm_ctx *ctx)
{
	struct drm_gpusvm_range *r;

retry:
	r = drm_gpusvm_range_find_or_insert(&svm->gpusvm, addr,
					    gpuva_start, gpuva_end, ctx);
	/*
	 * UMD doesn't set RO for some RO VMAs, but the drm gpu svm framework
	 * denies no RO flag range insert for RO VMAs, so treat
	 * -EPERM as an indication of RO and retry if not set.
	 */
	if (PTR_ERR_OR_ZERO(r) == -EPERM && !ctx->read_only) {
		ctx->read_only = true;
		goto retry;
	}

	if (IS_ERR(r))
		return ERR_CAST(r);

	return to_amdgpu_svm_range(r);
}

int amdgpu_svm_range_get_pages(struct amdgpu_svm *svm,
			       struct drm_gpusvm_range *range,
			       struct drm_gpusvm_ctx *ctx)
{
	int ret;

retry:
	ret = drm_gpusvm_range_get_pages(&svm->gpusvm, range, ctx);
	/*
	 * HMM returns -EPERM when write access is requested for a read-only
	 * VMA. Retry as read-only so the eventual GPU mapping follows the CPU
	 * VMA permissions.
	 */
	if (ret == -EPERM && !ctx->read_only) {
		ctx->read_only = true;
		goto retry;
	}

	if (ret == -EOPNOTSUPP) {
		AMDGPU_SVM_ERR(
			"range get pages -EOPNOTSUPP, evict and retry: gpuva=[0x%lx-0x%lx) ret=%d\n",
			drm_gpusvm_range_start(range),
			drm_gpusvm_range_end(range), ret);
		amdgpu_svm_range_evict(svm, range);
	}

	return ret;
}

void amdgpu_svm_range_evict(struct amdgpu_svm *svm,
			    struct drm_gpusvm_range *range)
{
	if (!range->pages.flags.has_devmem_pages)
		return;

	drm_gpusvm_range_evict(&svm->gpusvm, range);
}

int amdgpu_svm_range_update_mapping(struct amdgpu_svm *svm,
				    struct amdgpu_svm_range *range,
				    const struct amdgpu_svm_attrs *attrs,
				    bool read_only,
				    bool intr, bool wait,
				    bool flush_tlb)
{
	struct drm_exec exec;
	struct dma_fence *fence = NULL;
	unsigned int flags;
	int ret;

	ret = amdgpu_svm_range_lock_vm_pd(svm, &exec, intr);
	if (ret)
		return ret;

	flags = memalloc_noreclaim_save();
	drm_gpusvm_notifier_lock(&svm->gpusvm);

	if (!amdgpu_svm_range_pages_valid(svm, range)) {
		amdgpu_svm_range_invalidate_gpu_mapping(range);
		ret = -EAGAIN;
	} else {
		ret = amdgpu_svm_range_update_gpu_range(svm, range, attrs,
							read_only, flush_tlb,
							wait, wait ? &fence : NULL);
	}

	drm_gpusvm_notifier_unlock(&svm->gpusvm);
	memalloc_noreclaim_restore(flags);

	if (!ret && fence)
		dma_fence_wait(fence, intr);
	dma_fence_put(fence);

	if (!ret)
		ret = amdgpu_vm_update_pdes(svm->adev, svm->vm, false);

	if (!ret) {
		if (flush_tlb)
			svm->flush_tlb(svm);
		WRITE_ONCE(range->attr_flags, attrs->flags);
		WRITE_ONCE(range->gpu_mapped, true);
		range->validate_timestamp = ktime_get_boottime();
	}

	drm_exec_fini(&exec);
	return ret;
}

int
amdgpu_svm_range_map_attrs(struct amdgpu_svm *svm,
		       const struct amdgpu_svm_attrs *attrs,
		       unsigned long start, unsigned long end)
{
	unsigned long addr = start;
	int ret;
	bool devmem_possible = false; /* TODO: add migration */
	bool need_vram_migration = amdgpu_svm_attr_prefer_vram(svm, attrs);
	struct drm_gpusvm_ctx map_ctx = {
		.devmem_possible = devmem_possible,
		.devmem_only = need_vram_migration && devmem_possible,
		.check_pages_threshold = devmem_possible ? SZ_64K : 0,
	};

	while (addr < end) {
		struct amdgpu_svm_range *range;
		unsigned long next_addr;
		/* reset read_only every iteration, amdgpu_svm_range_find_or_insert may change it */
		map_ctx.read_only = !!(attrs->flags & AMDGPU_SVM_ATTR_BIT_GPU_RO);

		range = amdgpu_svm_range_find_or_insert(svm, addr,
							addr, end,
							&map_ctx);
		if (IS_ERR(range)) {
			AMDGPU_SVM_ERR(
				"failed to find/insert range for gpuva 0x%lx [0x%lx-0x%lx), ret=%ld\n",
				addr, start, end, PTR_ERR(range));
			return PTR_ERR(range);
		}

		next_addr = drm_gpusvm_range_end(&range->base);
		if (next_addr <= addr)
			return -EINVAL;

		if (amdgpu_svm_range_is_valid(svm, range, attrs)) {
			addr = next_addr;
			continue;
		}

		/* TODO: add migration */

		AMDGPU_SVM_RANGE_DEBUG(range, "GET PAGES");

		ret = amdgpu_svm_range_get_pages(svm, &range->base,
						 &map_ctx);
		if (ret) {
			AMDGPU_SVM_ERR("failed to get pages for range [0x%lx-0x%lx), ret=%d\n",
					drm_gpusvm_range_start(&range->base),
					drm_gpusvm_range_end(&range->base), ret);
			return ret;
		}

		AMDGPU_SVM_RANGE_DEBUG(range, "UPDATE MAPPING");

		ret = amdgpu_svm_range_update_mapping(svm, range, attrs,
						      map_ctx.read_only,
						      true, true,
						      true);
		if (ret) {
			AMDGPU_SVM_ERR("failed to update gpu map for range [0x%lx-0x%lx), ret=%d\n",
					drm_gpusvm_range_start(&range->base),
					drm_gpusvm_range_end(&range->base), ret);
			return ret;
		}

		addr = next_addr;
	}

	return 0;
}

void amdgpu_svm_range_remove(struct amdgpu_svm *svm,
			     struct amdgpu_svm_range *range,
			     struct drm_gpusvm_ctx *ctx)
{
	struct drm_gpusvm_range *base = &range->base;

	amdgpu_svm_assert_locked(svm);

	if (!base->pages.flags.unmapped && !base->pages.flags.partial_unmap)
		drm_gpusvm_range_unmap_pages(&svm->gpusvm, base, ctx);

	amdgpu_svm_range_invalidate_gpu_mapping(range);
	drm_gpusvm_range_remove(&svm->gpusvm, base);
}

bool
amdgpu_svm_range_notifier_event_begin(struct amdgpu_svm *svm,
				      struct drm_gpusvm_range *range,
				      const struct mmu_notifier_range *mmu_range)
{
	struct amdgpu_svm_range *svm_range = to_amdgpu_svm_range(range);
	unsigned long start_page, last_page;

	amdgpu_svm_assert_in_notifier(svm);

	AMDGPU_SVM_RANGE_DEBUG(svm_range, "NOTIFIER");

	if (range->pages.flags.unmapped || !svm_range->gpu_mapped)
		return false;

	AMDGPU_SVM_RANGE_DEBUG(svm_range, "NOTIFIER - EXECUTE");

	start_page = max(drm_gpusvm_range_start(range),
			 mmu_range->start) >> PAGE_SHIFT;
	last_page = (min(drm_gpusvm_range_end(range),
			 mmu_range->end) >> PAGE_SHIFT) - 1;

	amdgpu_svm_range_zap_ptes(svm, svm_range, start_page, last_page);
	amdgpu_svm_range_invalidate_gpu_mapping(svm_range);

	return true;
}

static void
amdgpu_svm_gc_enqueue(struct amdgpu_svm *svm,
		      struct amdgpu_svm_range *range,
		      unsigned long start_page, unsigned long last_page)
{
	if (atomic_read(&svm->exiting))
		return;

	spin_lock(&svm->work_lock);
	if (range->queue_state == AMDGPU_SVM_RANGE_NOT_QUEUED) {
		drm_gpusvm_range_get(&range->base);
		range->queue_state = AMDGPU_SVM_RANGE_IN_GC;
	}

	range->pending_start_page = min(range->pending_start_page, start_page);
	range->pending_last_page = max(range->pending_last_page, last_page);
	if (range->pending_ops == AMDGPU_SVM_RANGE_OP_NONE)
		list_add_tail(&range->work_node, &svm->gc.list);
	range->pending_ops |= AMDGPU_SVM_RANGE_OP_UNMAP;
	spin_unlock(&svm->work_lock);

	queue_work(svm->gc.wq, &svm->gc.work);
}

static void
amdgpu_svm_gc_add_range(struct amdgpu_svm *svm,
			struct amdgpu_svm_range *svm_range,
			const struct mmu_notifier_range *mmu_range)
{
	unsigned long start_page = max(drm_gpusvm_range_start(&svm_range->base),
				       mmu_range->start) >> PAGE_SHIFT;
	unsigned long last_page = (min(drm_gpusvm_range_end(&svm_range->base),
				       mmu_range->end) >> PAGE_SHIFT) - 1;

	AMDGPU_SVM_RANGE_DEBUG(svm_range, "GARBAGE COLLECTOR ADD");

	drm_gpusvm_range_set_unmapped(&svm_range->base, mmu_range);
	amdgpu_svm_gc_enqueue(svm, svm_range, start_page, last_page);
}

static void
amdgpu_svm_range_notifier_event_end(struct amdgpu_svm *svm,
				    struct drm_gpusvm_range *range,
				    const struct mmu_notifier_range *mmu_range)
{
	struct drm_gpusvm_ctx ctx = { .in_notifier = true, };

	amdgpu_svm_assert_in_notifier(svm);

	drm_gpusvm_range_unmap_pages(&svm->gpusvm, range, &ctx);
	if (mmu_range->event == MMU_NOTIFY_UNMAP)
		amdgpu_svm_gc_add_range(svm, to_amdgpu_svm_range(range),
					mmu_range);
}

int
amdgpu_svm_range_invalidate_interval(struct amdgpu_svm *svm,
				     unsigned long start_page,
				     unsigned long last_page)
{
	unsigned long start = start_page << PAGE_SHIFT;
	unsigned long end = (last_page + 1) << PAGE_SHIFT;
	struct drm_gpusvm_notifier *notifier, *next_notifier;
	struct drm_gpusvm_ctx ctx = { .in_notifier = false };
	struct drm_exec exec;
	bool needs_flush = false;
	int ret;

	amdgpu_svm_assert_locked(svm);

	ret = amdgpu_svm_range_lock_vm_pd(svm, &exec, true);
	if (ret)
		return ret;

	drm_gpusvm_for_each_notifier_safe(notifier, next_notifier, &svm->gpusvm,
					  start, end) {
		struct drm_gpusvm_range *range, *next_range;

		drm_gpusvm_for_each_range_safe(range, next_range, notifier,
					       start, end) {
			struct amdgpu_svm_range *svm_range = to_amdgpu_svm_range(range);
			unsigned long range_start = drm_gpusvm_range_start(range);
			unsigned long range_end = drm_gpusvm_range_end(range);
			unsigned long rs = range_start >> PAGE_SHIFT;
			unsigned long rl = (range_end >> PAGE_SHIFT) - 1;
			bool crosses_boundary;

			crosses_boundary = start > range_start || end < range_end;

			if (svm_range->gpu_mapped) {
				AMDGPU_SVM_RANGE_DEBUG(svm_range,
					crosses_boundary ? "ATTR DESTROY" :
							   "ATTR ZAP PTE");

				ret = amdgpu_svm_range_zap_ptes(svm, svm_range, rs, rl);
				if (ret < 0) {
					AMDGPU_SVM_TRACE(
						"attr invalidate PTE clear failed: ret=%d [0x%lx-0x%lx]\n",
						ret, rs, rl);
					drm_exec_fini(&exec);
					return ret;
				}
				needs_flush = true;
			}

			if (crosses_boundary) {
				/* remove ranges crossing the boundary so GPU fault
				 * creates new ranges bounded by the updated
				 * attr_range boundaries.
				 * Evict devmem-backed pages back to sysmem first
				 * so VRAM-resident data is not lost when the range
				 * is destroyed. No-op for sysmem-only ranges.
				 */
				amdgpu_svm_range_evict(svm, range);
				amdgpu_svm_range_remove(svm, svm_range, &ctx);
			} else {
				amdgpu_svm_range_invalidate_gpu_mapping(svm_range);
			}
		}
	}

	drm_exec_fini(&exec);

	if (needs_flush)
		svm->flush_tlb(svm);

	AMDGPU_SVM_TRACE("attr invalidate done [0x%lx-0x%lx]-0x%lx needs_flush=%d\n",
			 start_page, last_page, last_page - start_page + 1,
			 needs_flush ? 1 : 0);

	return 0;
}

bool
amdgpu_svm_range_dequeue_locked(struct amdgpu_svm *svm,
					struct list_head *work_list,
					struct amdgpu_svm_range_op_ctx *op_ctx)
{
	struct amdgpu_svm_range *range;

	lockdep_assert_held(&svm->work_lock);

	range = list_first_entry_or_null(work_list, struct amdgpu_svm_range,
					work_node);
	if (!range)
		return false;

	list_del_init(&range->work_node);
	range->queue_state = AMDGPU_SVM_RANGE_PROCESSING;

	op_ctx->range = range;
	op_ctx->start_page = range->pending_start_page;
	op_ctx->last_page = range->pending_last_page;
	op_ctx->pending_ops = range->pending_ops;

	range->pending_start_page = ULONG_MAX;
	range->pending_last_page = 0;
	range->pending_ops = AMDGPU_SVM_RANGE_OP_NONE;

	return true;
}

void
amdgpu_svm_range_put_if_dequeued(struct amdgpu_svm *svm,
				     struct amdgpu_svm_range *range)
{
	bool release_kref = false;
	bool queue_gc = false;

	spin_lock(&svm->work_lock);

	if (range->queue_state != AMDGPU_SVM_RANGE_PROCESSING) {
		spin_unlock(&svm->work_lock);
		return;
	}

	if (UNMAP_WORK(range->pending_ops)) {
		list_add_tail(&range->work_node, &svm->gc.list);
		range->queue_state = AMDGPU_SVM_RANGE_IN_GC;
		queue_gc = true;
	} else {
		range->queue_state = AMDGPU_SVM_RANGE_NOT_QUEUED;
		release_kref = true;
	}

	spin_unlock(&svm->work_lock);

	if (queue_gc)
		queue_work(svm->gc.wq, &svm->gc.work);
	if (release_kref)
		drm_gpusvm_range_put(&range->base);
}

void amdgpu_svm_capture_checkpoint_ts(struct amdgpu_svm *svm)
{
	struct amdgpu_device *adev = svm->adev;
	struct amdgpu_ih_ring *ih;
	uint32_t checkpoint_wptr;

	if (!adev->irq.retry_cam_enabled && adev->irq.ih1.ring_size) {
		ih = &adev->irq.ih1;
		checkpoint_wptr = amdgpu_ih_get_wptr(adev, ih);
		if (ih->rptr != checkpoint_wptr) {
			WRITE_ONCE(svm->checkpoint_ts,
				   amdgpu_ih_decode_iv_ts(adev, ih,
							  checkpoint_wptr, -1));
			return;
		}
	}

	ih = &adev->irq.ih_soft;
	checkpoint_wptr = amdgpu_ih_get_wptr(adev, ih);
	if (ih->rptr != checkpoint_wptr)
		WRITE_ONCE(svm->checkpoint_ts,
			   amdgpu_ih_decode_iv_ts(adev, ih,
						  checkpoint_wptr, -1));
}

void amdgpu_svm_range_invalidate(struct amdgpu_svm *svm,
				 struct drm_gpusvm_notifier *notifier,
				 const struct mmu_notifier_range *mmu_range,
				 struct drm_gpusvm_range *first,
				 uint64_t adj_start, uint64_t adj_end)
{
	struct drm_gpusvm_range *r;
	bool needs_flush = false;

	if (mmu_range->event == MMU_NOTIFY_UNMAP)
		amdgpu_svm_capture_checkpoint_ts(svm);

	r = first;
	drm_gpusvm_for_each_range(r, notifier, adj_start, adj_end)
		needs_flush |= amdgpu_svm_range_notifier_event_begin(svm, r,
								     mmu_range);
	if (needs_flush)
		svm->flush_tlb(svm);

	r = first;
	drm_gpusvm_for_each_range(r, notifier, adj_start, adj_end)
		amdgpu_svm_range_notifier_event_end(svm, r, mmu_range);
}
