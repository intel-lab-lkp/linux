// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "xe_vm_madvise.h"

#include <linux/nospec.h>
#include <drm/ttm/ttm_tt.h>
#include <drm/xe_drm.h>

#include "xe_bo.h"
#include "xe_gt_tlb_invalidation.h"
#include "xe_pt.h"
#include "xe_svm.h"

struct xe_vmas_in_madvise_range {
	u64 addr;
	u64 range;
	struct xe_vma **vmas;
	int num_vmas;
	bool has_svm_vmas;
	bool has_bo_vmas;
	bool has_userptr_vmas;
};

static int get_vmas(struct xe_vm *vm, struct xe_vmas_in_madvise_range *madvise_range)
{
	u64 addr = madvise_range->addr;
	u64 range = madvise_range->range;

	struct xe_vma  **__vmas;
	struct drm_gpuva *gpuva;
	int max_vmas = 8;

	lockdep_assert_held(&vm->lock);

	madvise_range->num_vmas = 0;
	madvise_range->vmas = kmalloc_array(max_vmas, sizeof(*madvise_range->vmas), GFP_KERNEL);
	if (!madvise_range->vmas)
		return -ENOMEM;

	vm_dbg(&vm->xe->drm, "VMA's in range: start=0x%016llx, end=0x%016llx", addr, addr + range);

	drm_gpuvm_for_each_va_range(gpuva, &vm->gpuvm, addr, addr + range) {
		struct xe_vma *vma = gpuva_to_vma(gpuva);

		if (xe_vma_bo(vma))
			madvise_range->has_bo_vmas = true;
		else if (xe_vma_is_cpu_addr_mirror(vma))
			madvise_range->has_svm_vmas = true;
		else if (xe_vma_is_userptr(vma))
			madvise_range->has_userptr_vmas = true;
		else
			XE_WARN_ON("UNEXPECTED VMA");

		if (madvise_range->num_vmas == max_vmas) {
			max_vmas <<= 1;
			__vmas = krealloc(madvise_range->vmas,
					  max_vmas * sizeof(*madvise_range->vmas),
					  GFP_KERNEL);
			if (!__vmas) {
				kfree(madvise_range->vmas);
				return -ENOMEM;
			}
			madvise_range->vmas = __vmas;
		}

		madvise_range->vmas[madvise_range->num_vmas] = vma;
		(madvise_range->num_vmas)++;
	}

	if (!madvise_range->num_vmas)
		kfree(madvise_range->vmas);

	vm_dbg(&vm->xe->drm, "madvise_range-num_vmas = %d\n", madvise_range->num_vmas);

	return 0;
}

static void madvise_preferred_mem_loc(struct xe_device *xe, struct xe_vm *vm,
				      struct xe_vma **vmas, int num_vmas,
				      struct drm_xe_madvise *op)
{
	/* Implementation pending */
}

static void madvise_atomic(struct xe_device *xe, struct xe_vm *vm,
			   struct xe_vma **vmas, int num_vmas,
			   struct drm_xe_madvise *op)
{
	int i;

	xe_assert(vm->xe, op->type == DRM_XE_VMA_ATTR_ATOMIC);
	xe_assert(vm->xe, op->atomic.val <= DRM_XE_VMA_ATOMIC_CPU);

	for (i = 0; i < num_vmas; i++)
		vmas[i]->attr.atomic_access = op->atomic.val;
	/*TODO: handle bo backed vmas */
}

static void madvise_pat_index(struct xe_device *xe, struct xe_vm *vm,
			      struct xe_vma **vmas, int num_vmas,
			      struct drm_xe_madvise *op)
{
	/* Implementation pending */
}

typedef void (*madvise_func)(struct xe_device *xe, struct xe_vm *vm,
			     struct xe_vma **vmas, int num_vmas,
			     struct drm_xe_madvise *op);

static const madvise_func madvise_funcs[] = {
	[DRM_XE_VMA_ATTR_PREFERRED_LOC] = madvise_preferred_mem_loc,
	[DRM_XE_VMA_ATTR_ATOMIC] = madvise_atomic,
	[DRM_XE_VMA_ATTR_PAT] = madvise_pat_index,
};

static void xe_zap_ptes_in_madvise_range(struct xe_vm *vm, u64 start, u64 end, u8 *tile_mask)
{
	struct drm_gpuva *gpuva;
	struct xe_tile *tile;
	u8 id;

	lockdep_assert_held_write(&vm->lock);

	if (dma_resv_wait_timeout(xe_vm_resv(vm), DMA_RESV_USAGE_BOOKKEEP,
				  false, MAX_SCHEDULE_TIMEOUT) <= 0)
		XE_WARN_ON(1);

	*tile_mask = xe_svm_ranges_zap_ptes_in_range(vm, start, end);

	drm_gpuvm_for_each_va_range(gpuva, &vm->gpuvm, start, end) {
		struct xe_vma *vma = gpuva_to_vma(gpuva);

		if (xe_vma_is_cpu_addr_mirror(vma))
			continue;

		if (xe_vma_is_userptr(vma)) {
			WARN_ON_ONCE(!dma_resv_test_signaled(xe_vm_resv(xe_vma_vm(vma)),
							     DMA_RESV_USAGE_BOOKKEEP));
		}

		for_each_tile(tile, vm->xe, id) {
			if (xe_pt_zap_ptes(tile, vma)) {
				*tile_mask |= BIT(id);
				vma->tile_invalidated |= BIT(id);
			}
		}
	}
}

static int xe_vm_invalidate_madvise_range(struct xe_vm *vm, u64 start, u64 end)
{
	u8 tile_mask = 0;

	xe_zap_ptes_in_madvise_range(vm, start, end, &tile_mask);
	if (!tile_mask)
		return 0;

	xe_device_wmb(vm->xe);

	return xe_vm_range_tilemask_tlb_invalidation(vm, start, end, tile_mask);
}

static int drm_xe_madvise_args_are_sane(struct xe_device *xe, const struct drm_xe_madvise *args)
{
	if (XE_IOCTL_DBG(xe, !args))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, !IS_ALIGNED(args->start, SZ_4K)))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, !IS_ALIGNED(args->range, SZ_4K)))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, args->range < SZ_4K))
		return -EINVAL;

	switch (args->type) {
	case DRM_XE_VMA_ATTR_ATOMIC:
		if (XE_IOCTL_DBG(xe, args->atomic.val > DRM_XE_VMA_ATOMIC_CPU))
			return -EINVAL;
		break;
	case DRM_XE_VMA_ATTR_PAT:
		/*TODO: Add valid pat check */
		break;
	case DRM_XE_VMA_ATTR_PREFERRED_LOC:
		if (XE_IOCTL_DBG(xe, args->preferred_mem_loc.migration_policy >
				     DRM_XE_MIGRATE_ONLY_SYSTEM_PAGES))
			return -EINVAL;
		break;
	default:
		if (XE_IOCTL_DBG(xe, 1))
			return -EINVAL;
	}

	if (XE_IOCTL_DBG(xe, args->reserved[0] || args->reserved[1]))
		return -EINVAL;

	return 0;
}

/**
 * xe_vm_madvise_ioctl - Handle MADVise ioctl for a VM
 * @dev: DRM device pointer
 * @data: Pointer to ioctl data (drm_xe_madvise*)
 * @file: DRM file pointer
 *
 * Handles the MADVISE ioctl to provide memory advice for vma's within
 * input range.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_vm_madvise_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_madvise *args = data;
	struct xe_vm *vm;
	struct xe_bo *bo;
	struct drm_exec exec;
	int err = 0;
	int attr_type;

	vm = xe_vm_lookup(xef, args->vm_id);
	if (XE_IOCTL_DBG(xe, !vm))
		return -EINVAL;

	if (drm_xe_madvise_args_are_sane(vm->xe, args))
		return -EINVAL;

	down_write(&vm->lock);

	if (XE_IOCTL_DBG(xe, xe_vm_is_closed_or_banned(vm))) {
		err = -ENOENT;
		goto unlock_vm;
	}

	xe_vm_alloc_madvise_vma(vm, args->start, args->range);

	struct xe_vmas_in_madvise_range madvise_range = {.addr = args->start,
							 .range =  args->range, };
	err = get_vmas(vm, &madvise_range);
	if (err || !madvise_range.num_vmas)
		goto unlock_vm;

	if (madvise_range.has_bo_vmas) {
		drm_exec_init(&exec, DRM_EXEC_IGNORE_DUPLICATES, 0);
		drm_exec_until_all_locked(&exec) {
			for (int i = 0; i < madvise_range.num_vmas; i++) {
				bo = xe_vma_bo(madvise_range.vmas[i]);
				if (!bo)
					continue;
				err = drm_exec_lock_obj(&exec, &bo->ttm.base);
				drm_exec_retry_on_contention(&exec);
				if (err)
					goto err_fini;
			}
		}
	}

	if (madvise_range.has_userptr_vmas)
		down_read(&vm->userptr.notifier_lock);

	if (madvise_range.has_svm_vmas)
		xe_svm_notifier_lock(vm);

	attr_type = array_index_nospec(args->type, ARRAY_SIZE(madvise_funcs));
	madvise_funcs[attr_type](xe, vm, madvise_range.vmas, madvise_range.num_vmas, args);

	kfree(madvise_range.vmas);
	madvise_range.vmas = NULL;

	err = xe_vm_invalidate_madvise_range(vm, args->start, args->start + args->range);

	if (madvise_range.has_svm_vmas)
		xe_svm_notifier_unlock(vm);

	if (madvise_range.has_userptr_vmas)
		up_read(&vm->userptr.notifier_lock);
err_fini:
	if (madvise_range.has_bo_vmas)
		drm_exec_fini(&exec);
unlock_vm:
	up_write(&vm->lock);
	xe_vm_put(vm);
	return err;
}
