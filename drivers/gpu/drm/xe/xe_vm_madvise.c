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
	s32 devmem_fd;
	u32 migration_policy;
	int i;

	xe_assert(vm->xe, ops.type == DRM_XE_VMA_ATTR_PREFERRED_LOC);
	vm_dbg(&xe->drm, "migration policy = %d, devmem_fd = %d\n",
	       ops.preferred_mem_loc.migration_policy,
	       ops.preferred_mem_loc.devmem_fd);

	devmem_fd = (s32)ops.preferred_mem_loc.devmem_fd;
	devmem_fd = (devmem_fd < 0) ? 0 : devmem_fd;

	migration_policy = ops.preferred_mem_loc.migration_policy;

	for (i = 0; i < num_vmas; i++) {
		vmas[i]->attr.preferred_loc.devmem_fd = devmem_fd;
		vmas[i]->attr.preferred_loc.migration_policy = migration_policy;
	}

	return 0;
}

static int madvise_atomic(struct xe_device *xe, struct xe_vm *vm,
			  struct xe_vma **vmas, int num_vmas,
			  struct drm_xe_madvise_ops ops)
{
	struct xe_bo *bo;
	int err, i;

	xe_assert(vm->xe, ops.type == DRM_XE_VMA_ATTR_ATOMIC);
	xe_assert(vm->xe, ops.atomic.val > DRM_XE_VMA_ATOMIC_UNDEFINED &&
		  ops.atomic.val <= DRM_XE_VMA_ATOMIC_CPU);
	vm_dbg(&xe->drm, "attr_value = %d", ops.atomic.val);

	for (i = 0; i < num_vmas; i++) {
		vmas[i]->attr.atomic_access = ops.atomic.val;

		bo = xe_vma_bo(vmas[i]);
		if (!bo)
			continue;

		if (XE_IOCTL_DBG(xe, ops.atomic.val == DRM_XE_VMA_ATOMIC_CPU &&
				 !(bo->flags & XE_BO_FLAG_SYSTEM)))
			return -EINVAL;

		if (XE_IOCTL_DBG(xe, ops.atomic.val == DRM_XE_VMA_ATOMIC_DEVICE &&
				 !(bo->flags & XE_BO_FLAG_VRAM0) &&
				     !(bo->flags & XE_BO_FLAG_VRAM1)))
			return -EINVAL;

		if (XE_IOCTL_DBG(xe, ops.atomic.val == DRM_XE_VMA_ATOMIC_GLOBAL &&
				 (!(bo->flags & XE_BO_FLAG_SYSTEM) ||
				      (!(bo->flags & XE_BO_FLAG_VRAM0) &&
				      !(bo->flags & XE_BO_FLAG_VRAM1)))))
			return -EINVAL;

		err = xe_bo_lock(bo, true);
		if (err)
			return err;
		bo->attr.atomic_access = ops.atomic.val;

		/* Invalidate cpu page table, so bo can migrate to smem in next access */
		if (bo->attr.atomic_access == DRM_XE_VMA_ATOMIC_CPU ||
		    bo->attr.atomic_access == DRM_XE_VMA_ATOMIC_GLOBAL)
			ttm_bo_unmap_virtual(&bo->ttm);

		xe_bo_unlock(bo);
	}
	return 0;
}

static int madvise_pat_index(struct xe_device *xe, struct xe_vm *vm,
			     struct xe_vma **vmas, int num_vmas,
			     struct drm_xe_madvise_ops ops)
{
	int i;

	xe_assert(vm->xe, ops.type == DRM_XE_VMA_ATTR_PAT);
	vm_dbg(&xe->drm, "attr_value = %d", ops.pat_index.val);

	for (i = 0; i < num_vmas; i++)
		vmas[i]->attr.pat_index = ops.pat_index.val;

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
	struct drm_gpusvm_notifier *notifier;
	struct drm_gpuva *gpuva;
	struct xe_svm_range *range;
	struct xe_tile *tile;
	u64 adj_start, adj_end;
	u8 id;

	lockdep_assert_held(&vm->lock);

	if (dma_resv_wait_timeout(xe_vm_resv(vm), DMA_RESV_USAGE_BOOKKEEP,
				  false, MAX_SCHEDULE_TIMEOUT) <= 0)
		XE_WARN_ON(1);

	down_write(&vm->svm.gpusvm.notifier_lock);

	drm_gpusvm_for_each_notifier(notifier, &vm->svm.gpusvm, start, end) {
		struct drm_gpusvm_range *r = NULL;

		adj_start = max(start, notifier->itree.start);
		adj_end = min(end, notifier->itree.last + 1);
		drm_gpusvm_for_each_range(r, notifier, adj_start, adj_end) {
			range = to_xe_range(r);
			for_each_tile(tile, vm->xe, id) {
				if (xe_pt_zap_ptes_range(tile, vm, range)) {
					*tile_mask |= BIT(id);
					range->tile_invalidated |= BIT(id);
				}
			}
		}
	}

	up_write(&vm->svm.gpusvm.notifier_lock);

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

static void xe_vm_invalidate_madvise_range(struct xe_vm *vm, u64 start, u64 end)
{
	struct xe_gt_tlb_invalidation_fence
		fence[XE_MAX_TILES_PER_DEVICE * XE_MAX_GT_PER_TILE];
	struct xe_tile *tile;
	u32 fence_id = 0;
	u8 tile_mask = 0;
	u8 id;

	xe_zap_ptes_in_madvise_range(vm, start, end, &tile_mask);
	if (!tile_mask)
		return;

	xe_device_wmb(vm->xe);

	for_each_tile(tile, vm->xe, id) {
		if (tile_mask & BIT(id)) {
			int err;

			xe_gt_tlb_invalidation_fence_init(tile->primary_gt,
							  &fence[fence_id], true);

			err = xe_gt_tlb_invalidation_range(tile->primary_gt,
							   &fence[fence_id],
							   start,
							   end,
							   vm->usm.asid);
			if (WARN_ON_ONCE(err < 0))
				goto wait;
			++fence_id;

			if (!tile->media_gt)
				continue;

			xe_gt_tlb_invalidation_fence_init(tile->media_gt,
							  &fence[fence_id], true);

			err = xe_gt_tlb_invalidation_range(tile->media_gt,
							   &fence[fence_id],
							   start,
							   end,
							   vm->usm.asid);
			if (WARN_ON_ONCE(err < 0))
				goto wait;
			++fence_id;
		}
	}

wait:
	for (id = 0; id < fence_id; ++id)
		xe_gt_tlb_invalidation_fence_wait(&fence[id]);
}

static int input_ranges_same(struct drm_xe_madvise_ops *old,
			     struct drm_xe_madvise_ops *new)
{
	return (new->start == old->start && new->range == old->range);
}

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

	if (XE_IOCTL_DBG(xe, args->num_ops < 1))
		return -EINVAL;

	vm = xe_vm_lookup(xef, args->vm_id);
	if (XE_IOCTL_DBG(xe, !vm))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, !xe_vm_in_fault_mode(vm))) {
		err = -EINVAL;
		goto put_vm;
	}

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
		if (!advs_ops)
			return args->num_ops > 1 ? -ENOBUFS : -ENOMEM;

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
			goto unlock_vm;
		}

		attr_type = array_index_nospec(advs_ops[i].type, ARRAY_SIZE(madvise_funcs));
		err = madvise_funcs[attr_type](xe, vm, vmas, num_vmas, advs_ops[i]);

		kfree(vmas);
		vmas = NULL;

		if (err)
			break;
	}

	for (i = 0; i < args->num_ops; i++) {
		for (j = i + 1; j < args->num_ops; ++j) {
			if (input_ranges_same(&advs_ops[j], &advs_ops[i]))
				break;
		}
		xe_vm_invalidate_madvise_range(vm, advs_ops[i].start,
					       advs_ops[i].start + advs_ops[i].range);
	}
free_advs_ops:
	if (args->num_ops > 1)
		kvfree(advs_ops);
unlock_vm:
	up_write(&vm->lock);
put_vm:
	xe_vm_put(vm);
	return err;
}
