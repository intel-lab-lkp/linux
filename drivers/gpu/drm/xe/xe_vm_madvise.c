// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include "xe_vm_madvise.h"

#include <linux/nospec.h>
#include <drm/ttm/ttm_tt.h>
#include <drm/xe_drm.h>

#include "xe_bo.h"
#include "xe_gt_tlb_invalidation.h"
#include "xe_pt.h"
#include "xe_svm.h"

static struct xe_vma **get_vmas(struct xe_vm *vm, int *num_vmas,
				u64 addr, u64 range)
{
	struct xe_vma **vmas, **__vmas;
	struct drm_gpuva *gpuva;
	int max_vmas = 8;

	lockdep_assert_held(&vm->lock);

	*num_vmas = 0;
	vmas = kmalloc_array(max_vmas, sizeof(*vmas), GFP_KERNEL);
	if (!vmas)
		return NULL;

	vm_dbg(&vm->xe->drm, "VMA's in range: start=0x%016llx, end=0x%016llx", addr, addr + range);

	drm_gpuvm_for_each_va_range(gpuva, &vm->gpuvm, addr, addr + range) {
		struct xe_vma *vma = gpuva_to_vma(gpuva);

		if (*num_vmas == max_vmas) {
			max_vmas <<= 1;
			__vmas = krealloc(vmas, max_vmas * sizeof(*vmas), GFP_KERNEL);
			if (!__vmas) {
				kfree(vmas);
				return NULL;
			}
			vmas = __vmas;
		}

		vmas[*num_vmas] = vma;
		(*num_vmas)++;
	}

	vm_dbg(&vm->xe->drm, "*num_vmas = %d\n", *num_vmas);

	if (!*num_vmas) {
		kfree(vmas);
		return NULL;
	}

	return vmas;
}

static int madvise_preferred_mem_loc(struct xe_device *xe, struct xe_vm *vm,
				     struct xe_vma **vmas, int num_vmas,
				     struct drm_xe_madvise_ops ops)
{
	/* Implementation pending */
	return 0;
}

static int madvise_atomic(struct xe_device *xe, struct xe_vm *vm,
			  struct xe_vma **vmas, int num_vmas,
			  struct drm_xe_madvise_ops ops)
{
	/* Implementation pending */
	return 0;
}

static int madvise_pat_index(struct xe_device *xe, struct xe_vm *vm,
			     struct xe_vma **vmas, int num_vmas,
			     struct drm_xe_madvise_ops ops)
{
	/* Implementation pending */
	return 0;
}

static int madvise_purgeable_state(struct xe_device *xe, struct xe_vm *vm,
				   struct xe_vma **vmas, int num_vmas,
				   struct drm_xe_madvise_ops ops)
{
	/* Implementation pending */
	return 0;
}

typedef int (*madvise_func)(struct xe_device *xe, struct xe_vm *vm,
			    struct xe_vma **vmas, int num_vmas, struct drm_xe_madvise_ops ops);

static const madvise_func madvise_funcs[] = {
	[DRM_XE_VMA_ATTR_PREFERRED_LOC] = madvise_preferred_mem_loc,
	[DRM_XE_VMA_ATTR_ATOMIC] = madvise_atomic,
	[DRM_XE_VMA_ATTR_PAT] = madvise_pat_index,
	[DRM_XE_VMA_ATTR_PURGEABLE_STATE] = madvise_purgeable_state,
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
			WARN_ON_ONCE(!mmu_interval_check_retry
				    (&to_userptr_vma(vma)->userptr.notifier,
				     to_userptr_vma(vma)->userptr.notifier_seq));

			WARN_ON_ONCE(!dma_resv_test_signaled(xe_vm_resv(xe_vma_vm(vma)),
							     DMA_RESV_USAGE_BOOKKEEP));
		}

		if (xe_vma_bo(vma))
			xe_bo_lock(xe_vma_bo(vma), false);

		for_each_tile(tile, vm->xe, id) {
			if (xe_pt_zap_ptes(tile, vma))
				*tile_mask |= BIT(id);
		}

		if (xe_vma_bo(vma))
			xe_bo_unlock(xe_vma_bo(vma));
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

static int input_ranges_same(struct drm_xe_madvise_ops *old,
			     struct drm_xe_madvise_ops *new)
{
	return (new->start == old->start && new->range == old->range);
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
	struct drm_xe_madvise_ops *advs_ops;
	struct drm_xe_madvise *args = data;
	struct xe_vm *vm;
	struct xe_vma **vmas = NULL;
	int num_vmas, err = 0;
	int i, j, attr_type;
	bool needs_invalidation;

	if (XE_IOCTL_DBG(xe, args->num_ops < 1))
		return -EINVAL;

	vm = xe_vm_lookup(xef, args->vm_id);
	if (XE_IOCTL_DBG(xe, !vm))
		return -EINVAL;

	down_write(&vm->lock);

	if (XE_IOCTL_DBG(xe, xe_vm_is_closed_or_banned(vm))) {
		err = -ENOENT;
		goto unlock_vm;
	}

	if (args->num_ops > 1) {
		u64 __user *madvise_user = u64_to_user_ptr(args->vector_of_ops);

		advs_ops = kvmalloc_array(args->num_ops, sizeof(struct drm_xe_madvise_ops),
					  GFP_KERNEL | __GFP_ACCOUNT |
					  __GFP_RETRY_MAYFAIL | __GFP_NOWARN);
		if (!advs_ops) {
			err =  args->num_ops > 1 ? -ENOBUFS : -ENOMEM;
			goto unlock_vm;
		}

		err = __copy_from_user(advs_ops, madvise_user,
				       sizeof(struct drm_xe_madvise_ops) *
				       args->num_ops);
		if (XE_IOCTL_DBG(xe, err)) {
			err = -EFAULT;
			goto free_advs_ops;
		}
	} else {
		advs_ops = &args->ops;
	}

	for (i = 0; i < args->num_ops; i++) {
		xe_vm_alloc_madvise_vma(vm, advs_ops[i].start, advs_ops[i].range);

		vmas = get_vmas(vm, &num_vmas, advs_ops[i].start, advs_ops[i].range);
		if (!vmas) {
			err = -ENOMEM;
			goto free_advs_ops;
		}

		attr_type = array_index_nospec(advs_ops[i].type, ARRAY_SIZE(madvise_funcs));
		err = madvise_funcs[attr_type](xe, vm, vmas, num_vmas, advs_ops[i]);

		kfree(vmas);
		vmas = NULL;

		if (err)
			goto free_advs_ops;
	}

	for (i = 0; i < args->num_ops; i++) {
		needs_invalidation = true;
		for (j = i + 1; j < args->num_ops; ++j) {
			if (input_ranges_same(&advs_ops[j], &advs_ops[i])) {
				needs_invalidation = false;
				break;
			}
		}
		if (needs_invalidation) {
			err = xe_vm_invalidate_madvise_range(vm, advs_ops[i].start,
							     advs_ops[i].start + advs_ops[i].range);
			if (err)
				goto free_advs_ops;
		}
	}

free_advs_ops:
	if (args->num_ops > 1)
		kvfree(advs_ops);
unlock_vm:
	up_write(&vm->lock);
	xe_vm_put(vm);
	return err;
}
