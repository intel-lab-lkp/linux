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
#include "amdgpu_svm_fault.h"
#include "amdgpu_svm_range.h"
#include "amdgpu.h"
#include "amdgpu_vm.h"
#include "amdgpu_gmc.h"
#include "amdgpu_ih.h"

#include <drm/drm_exec.h>
#include <drm/drm_gpusvm.h>

#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>

#if IS_ENABLED(CONFIG_DRM_AMDGPU_SVM)

#define AMDGPU_SVM_RANGE_RETRY_FAULT_PENDING	(2UL * NSEC_PER_MSEC)

static int fault_get_unregistered_attrs(struct amdgpu_svm *svm,
					    unsigned long fault_addr,
					    unsigned long attr_start_page,
					    unsigned long attr_last_page,
					    struct amdgpu_svm_attr_range **out)
{
	struct amdgpu_svm_attr_tree *attr_tree = svm->attr_tree;
	struct amdgpu_svm_attr_range *range;
	struct amdgpu_svm_attrs attrs;
	struct mm_struct *mm = svm->gpusvm.mm;
	struct vm_area_struct *vma;
	unsigned long fault_page = fault_addr >> PAGE_SHIFT;
	unsigned long start_page, last_page;
	unsigned long vma_start_page, vma_last_page;
	unsigned long bo_start = 0, bo_last = 0;
	int r;

	amdgpu_svm_attr_set_default(svm, &attrs);

	mmap_read_lock(mm);

	vma = amdgpu_svm_check_vma(mm, fault_addr);
	if (IS_ERR(vma)) {
		mmap_read_unlock(mm);
		AMDGPU_SVM_ERR("get_unregistered_attrs: invalid VMA for fault_addr=0x%lx\n",
		       fault_addr);
		return PTR_ERR(vma);
	}
	vma_start_page = vma->vm_start >> PAGE_SHIFT;
	vma_last_page = (vma->vm_end >> PAGE_SHIFT) - 1;

	if (vma_is_initial_heap(vma) || vma_is_initial_stack(vma))
		attrs.preferred_loc = AMDGPU_SVM_LOCATION_SYSMEM;

	mmap_read_unlock(mm);

	start_page = max_t(unsigned long, vma_start_page,
			   ALIGN_DOWN(fault_page, 1UL << attrs.granularity));
	last_page = min_t(unsigned long, vma_last_page,
			  ALIGN(fault_page + 1, 1UL << attrs.granularity) - 1);

	start_page = max(start_page, attr_start_page);
	last_page = min(last_page, attr_last_page);

	r = amdgpu_svm_attr_check_vm_bo(attr_tree, start_page, last_page,
					&bo_start, &bo_last);
	if (r == -EADDRINUSE) {
		if (fault_page >= bo_start && fault_page <= bo_last)
			return -EFAULT;

		/* Narrow to single page if expanded range overlaps BO */
		start_page = fault_page;
		last_page = fault_page;
	} else if (r) {
		return r;
	}

	mutex_lock(&attr_tree->lock);
	range = amdgpu_svm_attr_range_alloc(start_page, last_page, &attrs);
	if (!range) {
		mutex_unlock(&attr_tree->lock);
		return -ENOMEM;
	}
	amdgpu_svm_attr_range_insert_locked(attr_tree, range);
	mutex_unlock(&attr_tree->lock);

	AMDGPU_SVM_TRACE(
		"Created unregistered range for fault_addr=0x%lx: attr range=[0x%lx-0x%lx] size: 0x%lx attrs={preferred_loc=%d, prefetch_loc=%d, flags=0x%x, granularity=%u, access=%u}\n",
		fault_addr, amdgpu_svm_attr_start_page(range),
		amdgpu_svm_attr_last_page(range) + 1,
		amdgpu_svm_attr_last_page(range) -
			amdgpu_svm_attr_start_page(range) + 1,
		range->attrs.preferred_loc, range->attrs.prefetch_loc,
		range->attrs.flags, range->attrs.granularity,
		range->attrs.access);

	*out = range;
	return 0;
}

static int fault_check_allowed(struct amdgpu_svm *svm,
				   unsigned long fault_addr, bool write_fault)
{
	struct mm_struct *mm = svm->gpusvm.mm;
	struct vm_area_struct *vma;
	unsigned long requested = VM_READ;
	int ret = 0;

	if (write_fault)
		requested |= VM_WRITE;

	mmap_read_lock(mm);
	vma = vma_lookup(mm, fault_addr);
	if (vma && (vma->vm_flags & requested) != requested) {
		AMDGPU_SVM_ERR("fault addr 0x%lx no %s permission\n",
			 fault_addr, write_fault ? "write" : "read");
		ret = -EPERM;
	}
	mmap_read_unlock(mm);

	return ret;
}

static int fault_map_range(struct amdgpu_svm *svm,
			       unsigned long fault_addr,
			       const struct amdgpu_svm_attr_range *attr_range,
			       bool write_fault)
{
	const struct amdgpu_svm_attrs *attrs = &attr_range->attrs;
	bool devmem_possible = false; /* TODO: add migration */
	bool need_vram_migration = amdgpu_svm_attr_prefer_vram(svm, attrs);
	struct drm_gpusvm_ctx map_ctx = {
		.read_only = !!(attrs->flags & AMDGPU_SVM_ATTR_BIT_GPU_RO),
		.devmem_possible = devmem_possible,
		.check_pages_threshold = devmem_possible ? SZ_64K : 0,
		.devmem_only = need_vram_migration && devmem_possible,
		.timeslice_ms = need_vram_migration && devmem_possible ? 5 : 0,
	};
	struct amdgpu_svm_range *range;
	ktime_t timestamp = ktime_get_boottime();
	int retry_count = 3;
	int ret;

	amdgpu_svm_assert_locked(svm);
	WARN_ON(!svm->xnack_enabled);

retry:
	ret = amdgpu_svm_garbage_collector(svm);
	if (ret) {
		AMDGPU_SVM_ERR(
			"fault garbage collector failed: ret=%d, fault_addr=0x%lx\n",
			ret, fault_addr);
		return ret;
	}

	ret = fault_check_allowed(svm, fault_addr, write_fault);
	if (ret)
		return ret;

	range = amdgpu_svm_range_find_or_insert(
		svm, fault_addr,
		amdgpu_svm_attr_start(attr_range),
		amdgpu_svm_attr_end(attr_range),
		&map_ctx);
	if (IS_ERR(range)) {
		ret = PTR_ERR(range);
		AMDGPU_SVM_ERR("map_fault: range_find_or_insert failed: fault=0x%lx ret=%d\n",
				 fault_addr, ret);
		/*
		 * -EINVAL: fault_addr out of gpusvm range, or no chunk size
		 *          fits within VMA/notifier/attr_range bounds.
		 * -EFAULT: mmget_not_zero failed.
		 * -ENOENT: No VMA at fault_addr.
		 * -ENOMEM: Notifier or range allocation failed.
		 */
		if (ret == -EFAULT || ret == -ENOENT) {
			AMDGPU_SVM_ERR("no vma or mm is dying: 0x%lx, ret=%d\n",
					 fault_addr, ret);
			ret = 0;
		}

		return ret;
	}

	if (write_fault && map_ctx.read_only) {
		AMDGPU_SVM_WARN("write fault on read-only range: fault=0x%lx range=[0x%lx-0x%lx)\n",
				 fault_addr, drm_gpusvm_range_start(&range->base),
				 drm_gpusvm_range_end(&range->base));
	}

	if (ktime_before(timestamp, ktime_add_ns(range->validate_timestamp,
					 AMDGPU_SVM_RANGE_RETRY_FAULT_PENDING))) {
		AMDGPU_SVM_TRACE("already restored, skip: fault=0x%lx range=[0x%lx-0x%lx)\n",
				 fault_addr, drm_gpusvm_range_start(&range->base),
				 drm_gpusvm_range_end(&range->base));
		goto out;
	}

	if (amdgpu_svm_range_is_valid(svm, range, attrs)) {
		AMDGPU_SVM_TRACE("valid range, skip: fault=0x%lx range=[0x%lx-0x%lx)\n",
				 fault_addr, drm_gpusvm_range_start(&range->base),
				 drm_gpusvm_range_end(&range->base));
		goto out;
	}

	AMDGPU_SVM_RANGE_DEBUG(range, "PAGE FAULT");
	/* TODO: add migration*/

	AMDGPU_SVM_RANGE_DEBUG(range, "GET PAGES");
	ret = amdgpu_svm_range_get_pages(svm, &range->base, &map_ctx);
	if (ret == -EOPNOTSUPP || ret == -EFAULT) {
		/*
		 * -EOPNOTSUPP  Mixed page types within range.
		 * -EFAULT      (a) mm is dying.
		 *              (b) range was unmapped.
		 *              (c) DMA mapping failed.
		 *              (d) devmem_only requested but system page encountered.
		 *              (e) hmm_range_fault: no VMA, page fault error, bad pte/pmd.
		 * -EBUSY       HMM retry loop timed out.
		 * -ENOMEM      PFN or DMA address array allocation failed.
		 * -EINVAL      hmm_range_fault: invalid VMA type.
		 */
		map_ctx.timeslice_ms <<= 1;
		if (!map_ctx.devmem_only && --retry_count > 0) {
			AMDGPU_SVM_ERR(
				"retry: get_pages failed %d, left=%d: fault=0x%lx range=[0x%lx-0x%lx)\n",
				ret, retry_count, fault_addr,
				drm_gpusvm_range_start(&range->base),
				drm_gpusvm_range_end(&range->base));
			goto retry;
		} else {
			AMDGPU_SVM_ERR(
				"map_fault: get_pages failed %d, devmem fallback allowed but no devmem pages: fault=0x%lx range=[0x%lx-0x%lx)\n",
				ret, fault_addr,
				drm_gpusvm_range_start(&range->base),
				drm_gpusvm_range_end(&range->base));
		}
	}

	if (ret == -EPERM) {
		AMDGPU_SVM_ERR("get_pages -EPERM: fault=0x%lx range=[0x%lx-0x%lx)\n",
			       fault_addr, drm_gpusvm_range_start(&range->base),
				       drm_gpusvm_range_end(&range->base));
		return ret;
	}

	if (ret) {
		AMDGPU_SVM_RANGE_DEBUG(range, "PAGE FAULT - FAIL PAGE COLLECT");
		goto out;
	}

	AMDGPU_SVM_RANGE_DEBUG(range, "PAGE FAULT - GPU MAP");

	ret = amdgpu_svm_range_update_mapping(svm, range, attrs,
					      map_ctx.read_only,
					      false, false, false);

	if (ret)
		goto err_out;

out:
	return 0;

err_out:
	if (ret == -EAGAIN && --retry_count > 0) {
		map_ctx.timeslice_ms <<= 1;
		AMDGPU_SVM_RANGE_DEBUG(range, "PAGE FAULT - RETRY GPU MAP");
		goto retry;
	}

	return ret;
}

int amdgpu_svm_handle_fault(struct amdgpu_device *adev, uint32_t pasid,
			    uint64_t fault_page, uint64_t ts,
			    bool write_fault)
{
	struct amdgpu_svm *svm;
	struct amdgpu_svm_attr_range *attr_range;
	unsigned long attr_start_page, attr_last_page;
	uint64_t fault_addr = fault_page << PAGE_SHIFT;
	uint64_t ckpt;
	int ret;

	svm = amdgpu_svm_lookup_by_pasid(adev, pasid);
	if (!svm) {
		AMDGPU_SVM_ERR("handle_fault: no SVM context for pasid %u\n", pasid);
		return -EOPNOTSUPP;
	}

	if (atomic_read(&svm->exiting)) {
		AMDGPU_SVM_ERR("handle_fault: SVM context is exiting for pasid %u\n", pasid);
		ret = -EAGAIN;
		goto out_put;
	}

	if (!svm->xnack_enabled) {
		AMDGPU_SVM_ERR("handle_fault: xnack not enabled for pasid %u\n",
			       pasid);
		ret = -EOPNOTSUPP;
		goto out_put;
	}

	ckpt = READ_ONCE(svm->checkpoint_ts);
	if (ckpt != 0) {
		if (amdgpu_ih_ts_after_or_equal(ts, ckpt)) {
			AMDGPU_SVM_TRACE(
			"handle_fault: draining stale retry fault, drop fault 0x%llx ts=%llu checkpoint=%llu\n",
				fault_addr, ts, ckpt);
			amdgpu_gmc_filter_faults_remove(
				adev, fault_page, pasid);
			ret = 0;
			goto out_put;
		} else {
			WRITE_ONCE(svm->checkpoint_ts, 0);
		}
	}

	amdgpu_svm_lock(svm);

	mutex_lock(&svm->attr_tree->lock);
	attr_range = amdgpu_svm_attr_get_bounds_locked(svm->attr_tree,
						       fault_page,
						       &attr_start_page, &attr_last_page);
	mutex_unlock(&svm->attr_tree->lock);
	if (!attr_range) {
		ret = fault_get_unregistered_attrs(svm, fault_addr,
							      attr_start_page,
							      attr_last_page,
							      &attr_range);
		if (ret) {
			if (ret == -EFAULT)
				goto out_no_vma;
			goto out_unlock;
		}
	}
	ret = fault_map_range(svm, fault_addr, attr_range,
					 write_fault);

	if (ret == -EAGAIN) {
		AMDGPU_SVM_ERR("handle_fault: got -EAGAIN: fault=0x%llx\n",
			       fault_addr);
		amdgpu_gmc_filter_faults_remove(adev, fault_page, pasid);
		ret = 0;
	}

	goto out_unlock;

out_no_vma:
	AMDGPU_SVM_ERR("handle_fault: no VMA for fault=0x%llx (stale retry or GPU NULL deref)\n",
		 fault_addr);
	ret = 0;

out_unlock:
	amdgpu_svm_unlock(svm);

out_put:
	amdgpu_svm_put(svm);
	return ret;
}

#endif /* CONFIG_DRM_AMDGPU_SVM */
