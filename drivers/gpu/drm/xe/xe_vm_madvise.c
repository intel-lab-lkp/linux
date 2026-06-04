// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "xe_vm_madvise.h"

#include <linux/nospec.h>
#include <linux/maple_tree.h>
#include <linux/workqueue.h>
#include <drm/xe_drm.h>

#include "xe_bo.h"
#include "xe_pat.h"
#include "xe_pt.h"
#include "xe_svm.h"
#include "xe_tlb_inval.h"
#include "xe_vm.h"
#include "xe_macros.h"

/* Lockdep class for teardown_rwsem */
static struct lock_class_key xe_madvise_teardown_key;

struct xe_vmas_in_madvise_range {
	u64 addr;
	u64 range;
	struct xe_vma **vmas;
	int num_vmas;
	bool has_bo_vmas;
	bool has_svm_userptr_vmas;
};

/**
 * struct xe_madvise_details - Argument to madvise_funcs
 * @dpagemap: Reference-counted pointer to a struct drm_pagemap.
 * @has_purged_bo: Track if any BO was purged (for purgeable state)
 * @retained_ptr: User pointer for retained value (for purgeable state)
 *
 * The madvise IOCTL handler may, in addition to the user-space
 * args, have additional info to pass into the madvise_func that
 * handles the madvise type. Use a struct_xe_madvise_details
 * for that and extend the struct as necessary.
 */
struct xe_madvise_details {
	struct drm_pagemap *dpagemap;
	bool has_purged_bo;
	u64 retained_ptr;
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
	madvise_range->vmas = kmalloc_objs(*madvise_range->vmas, max_vmas);
	if (!madvise_range->vmas)
		return -ENOMEM;

	vm_dbg(&vm->xe->drm, "VMA's in range: start=0x%016llx, end=0x%016llx", addr, addr + range);

	drm_gpuvm_for_each_va_range(gpuva, &vm->gpuvm, addr, addr + range) {
		struct xe_vma *vma = gpuva_to_vma(gpuva);

		if (xe_vma_bo(vma))
			madvise_range->has_bo_vmas = true;
		else if (xe_vma_is_cpu_addr_mirror(vma) || xe_vma_is_userptr(vma))
			madvise_range->has_svm_userptr_vmas = true;

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
				      struct drm_xe_madvise *op,
				      struct xe_madvise_details *details)
{
	int i;

	xe_assert(vm->xe, op->type == DRM_XE_MEM_RANGE_ATTR_PREFERRED_LOC);

	for (i = 0; i < num_vmas; i++) {
		struct xe_vma *vma = vmas[i];
		struct xe_vma_preferred_loc *loc = &vma->attr.preferred_loc;

		/*TODO: Extend attributes to bo based vmas */
		if ((loc->devmem_fd == op->preferred_mem_loc.devmem_fd &&
		     loc->migration_policy == op->preferred_mem_loc.migration_policy) ||
		    !xe_vma_is_cpu_addr_mirror(vma)) {
			vma->skip_invalidation = true;
		} else {
			vma->skip_invalidation = false;
			loc->devmem_fd = op->preferred_mem_loc.devmem_fd;
			/* Till multi-device support is not added migration_policy
			 * is of no use and can be ignored.
			 */
			loc->migration_policy = op->preferred_mem_loc.migration_policy;
			drm_pagemap_put(loc->dpagemap);
			loc->dpagemap = NULL;
			if (details->dpagemap)
				loc->dpagemap = drm_pagemap_get(details->dpagemap);
		}
	}
}

static void madvise_atomic(struct xe_device *xe, struct xe_vm *vm,
			   struct xe_vma **vmas, int num_vmas,
			   struct drm_xe_madvise *op,
			   struct xe_madvise_details *details)
{
	struct xe_bo *bo;
	int i;

	xe_assert(vm->xe, op->type == DRM_XE_MEM_RANGE_ATTR_ATOMIC);
	xe_assert(vm->xe, op->atomic.val <= DRM_XE_ATOMIC_CPU);

	for (i = 0; i < num_vmas; i++) {
		if (xe_vma_is_userptr(vmas[i]) &&
		    !(op->atomic.val == DRM_XE_ATOMIC_DEVICE &&
		      xe->info.has_device_atomics_on_smem)) {
			vmas[i]->skip_invalidation = true;
			continue;
		}

		if (vmas[i]->attr.atomic_access == op->atomic.val) {
			vmas[i]->skip_invalidation = true;
		} else {
			vmas[i]->skip_invalidation = false;
			vmas[i]->attr.atomic_access = op->atomic.val;
		}

		bo = xe_vma_bo(vmas[i]);
		if (!bo || bo->attr.atomic_access == op->atomic.val)
			continue;

		vmas[i]->skip_invalidation = false;
		xe_bo_assert_held(bo);
		bo->attr.atomic_access = op->atomic.val;

		/* Invalidate cpu page table, so bo can migrate to smem in next access */
		if (xe_bo_is_vram(bo) &&
		    (bo->attr.atomic_access == DRM_XE_ATOMIC_CPU ||
		     bo->attr.atomic_access == DRM_XE_ATOMIC_GLOBAL))
			ttm_bo_unmap_virtual(&bo->ttm);
	}
}

static void madvise_pat_index(struct xe_device *xe, struct xe_vm *vm,
			      struct xe_vma **vmas, int num_vmas,
			      struct drm_xe_madvise *op,
			      struct xe_madvise_details *details)
{
	int i;

	xe_assert(vm->xe, op->type == DRM_XE_MEM_RANGE_ATTR_PAT);

	for (i = 0; i < num_vmas; i++) {
		if (vmas[i]->attr.pat_index == op->pat_index.val) {
			vmas[i]->skip_invalidation = true;
		} else {
			vmas[i]->skip_invalidation = false;
			vmas[i]->attr.pat_index = op->pat_index.val;
		}
	}
}

/**
 * madvise_purgeable - Handle purgeable buffer object advice
 * @xe: XE device
 * @vm: VM
 * @vmas: Array of VMAs
 * @num_vmas: Number of VMAs
 * @op: Madvise operation
 * @details: Madvise details for return values
 *
 * Handles DONTNEED/WILLNEED/PURGED states. Tracks if any BO was purged
 * in details->has_purged_bo for later copy to userspace.
 */
static void madvise_purgeable(struct xe_device *xe, struct xe_vm *vm,
			      struct xe_vma **vmas, int num_vmas,
			      struct drm_xe_madvise *op,
			      struct xe_madvise_details *details)
{
	int i;

	xe_assert(vm->xe, op->type == DRM_XE_VMA_ATTR_PURGEABLE_STATE);

	for (i = 0; i < num_vmas; i++) {
		struct xe_bo *bo = xe_vma_bo(vmas[i]);

		if (!bo) {
			/* Purgeable state applies to BOs only, skip non-BO VMAs */
			vmas[i]->skip_invalidation = true;
			continue;
		}

		/* BO must be locked before modifying madv state */
		xe_bo_assert_held(bo);

		/*
		 * Once purged, always purged. Cannot transition back to WILLNEED.
		 * This matches i915 semantics where purged BOs are permanently invalid.
		 */
		if (xe_bo_is_purged(bo)) {
			details->has_purged_bo = true;
			vmas[i]->skip_invalidation = true;
			continue;
		}

		switch (op->purge_state_val.val) {
		case DRM_XE_VMA_PURGEABLE_STATE_WILLNEED:
			vmas[i]->skip_invalidation = true;
			/* Only act on a real DONTNEED -> WILLNEED transition. */
			if (vmas[i]->attr.purgeable_state == XE_MADV_PURGEABLE_DONTNEED) {
				vmas[i]->attr.purgeable_state = XE_MADV_PURGEABLE_WILLNEED;
				xe_bo_willneed_get_locked(bo);
			}
			break;
		case DRM_XE_VMA_PURGEABLE_STATE_DONTNEED:
			/*
			 * Don't zap PTEs at DONTNEED time -- pages are still
			 * alive. The zap happens in xe_bo_move_notify() right
			 * before the shrinker frees them.
			 */
			vmas[i]->skip_invalidation = true;

			/* Only act on a real WILLNEED -> DONTNEED transition. */
			if (vmas[i]->attr.purgeable_state == XE_MADV_PURGEABLE_WILLNEED) {
				vmas[i]->attr.purgeable_state = XE_MADV_PURGEABLE_DONTNEED;
				xe_bo_willneed_put_locked(bo);
			}
			break;
		default:
			/* Should never hit - values validated in madvise_args_are_sane() */
			xe_assert(vm->xe, 0);
			return;
		}
	}
}

typedef void (*madvise_func)(struct xe_device *xe, struct xe_vm *vm,
			     struct xe_vma **vmas, int num_vmas,
			     struct drm_xe_madvise *op,
			     struct xe_madvise_details *details);

static const madvise_func madvise_funcs[] = {
	[DRM_XE_MEM_RANGE_ATTR_PREFERRED_LOC] = madvise_preferred_mem_loc,
	[DRM_XE_MEM_RANGE_ATTR_ATOMIC] = madvise_atomic,
	[DRM_XE_MEM_RANGE_ATTR_PAT] = madvise_pat_index,
	[DRM_XE_VMA_ATTR_PURGEABLE_STATE] = madvise_purgeable,
};

static u8 xe_zap_ptes_in_madvise_range(struct xe_vm *vm, u64 start, u64 end)
{
	struct drm_gpuva *gpuva;
	struct xe_tile *tile;
	u8 id, tile_mask = 0;

	lockdep_assert_held_write(&vm->lock);

	/* Wait for pending binds */
	if (dma_resv_wait_timeout(xe_vm_resv(vm), DMA_RESV_USAGE_BOOKKEEP,
				  false, MAX_SCHEDULE_TIMEOUT) <= 0)
		XE_WARN_ON(1);

	drm_gpuvm_for_each_va_range(gpuva, &vm->gpuvm, start, end) {
		struct xe_vma *vma = gpuva_to_vma(gpuva);

		if (vma->skip_invalidation || xe_vma_is_null(vma))
			continue;

		if (xe_vma_is_cpu_addr_mirror(vma)) {
			tile_mask |= xe_svm_ranges_zap_ptes_in_range(vm,
								      xe_vma_start(vma),
								      xe_vma_end(vma));
		} else {
			for_each_tile(tile, vm->xe, id) {
				if (xe_pt_zap_ptes(tile, vma)) {
					tile_mask |= BIT(id);

					/*
					 * WRITE_ONCE pairs with READ_ONCE
					 * in xe_vm_has_valid_gpu_mapping()
					 */
					WRITE_ONCE(vma->tile_invalidated,
						   vma->tile_invalidated | BIT(id));
				}
			}
		}
	}

	return tile_mask;
}

static int xe_vm_invalidate_madvise_range(struct xe_vm *vm, u64 start, u64 end)
{
	u8 tile_mask = xe_zap_ptes_in_madvise_range(vm, start, end);
	struct xe_tlb_inval_batch batch;
	int err;

	if (!tile_mask)
		return 0;

	xe_device_wmb(vm->xe);

	err = xe_tlb_inval_range_tilemask_submit(vm->xe, vm->usm.asid, start, end,
						 tile_mask, &batch);
	if (!err)
		xe_tlb_inval_batch_wait(&batch);

	return err;
}

static bool madvise_args_are_sane(struct xe_device *xe, const struct drm_xe_madvise *args)
{
	if (XE_IOCTL_DBG(xe, !args))
		return false;

	if (XE_IOCTL_DBG(xe, !IS_ALIGNED(args->start, SZ_4K)))
		return false;

	if (XE_IOCTL_DBG(xe, !IS_ALIGNED(args->range, SZ_4K)))
		return false;

	if (XE_IOCTL_DBG(xe, args->range < SZ_4K))
		return false;

	switch (args->type) {
	case DRM_XE_MEM_RANGE_ATTR_PREFERRED_LOC:
	{
		s32 fd = (s32)args->preferred_mem_loc.devmem_fd;

		if (XE_IOCTL_DBG(xe, fd < DRM_XE_PREFERRED_LOC_DEFAULT_SYSTEM))
			return false;

		if (XE_IOCTL_DBG(xe, fd <= DRM_XE_PREFERRED_LOC_DEFAULT_DEVICE &&
				 args->preferred_mem_loc.region_instance != 0))
			return false;

		if (XE_IOCTL_DBG(xe, args->preferred_mem_loc.migration_policy >
				     DRM_XE_MIGRATE_ONLY_SYSTEM_PAGES))
			return false;

		if (XE_IOCTL_DBG(xe, args->preferred_mem_loc.reserved))
			return false;
		break;
	}
	case DRM_XE_MEM_RANGE_ATTR_ATOMIC:
		if (XE_IOCTL_DBG(xe, args->atomic.val > DRM_XE_ATOMIC_CPU))
			return false;

		if (XE_IOCTL_DBG(xe, args->atomic.pad))
			return false;

		if (XE_IOCTL_DBG(xe, args->atomic.reserved))
			return false;

		break;
	case DRM_XE_MEM_RANGE_ATTR_PAT:
	{
		u16 pat_index, coh_mode;

		if (XE_IOCTL_DBG(xe, args->pat_index.val >= xe->pat.n_entries))
			return false;

		pat_index = array_index_nospec(args->pat_index.val, xe->pat.n_entries);
		coh_mode = xe_pat_index_get_coh_mode(xe, pat_index);
		if (XE_IOCTL_DBG(xe, !coh_mode))
			return false;

		if (XE_WARN_ON(coh_mode > XE_COH_2WAY))
			return false;

		if (XE_IOCTL_DBG(xe, args->pat_index.pad))
			return false;

		if (XE_IOCTL_DBG(xe, args->pat_index.reserved))
			return false;
		break;
	}
	case DRM_XE_VMA_ATTR_PURGEABLE_STATE:
	{
		u32 val = args->purge_state_val.val;

		if (XE_IOCTL_DBG(xe, !(val == DRM_XE_VMA_PURGEABLE_STATE_WILLNEED ||
				       val == DRM_XE_VMA_PURGEABLE_STATE_DONTNEED)))
			return false;

		if (XE_IOCTL_DBG(xe, args->purge_state_val.pad))
			return false;

		break;
	}
	default:
		if (XE_IOCTL_DBG(xe, 1))
			return false;
	}

	if (XE_IOCTL_DBG(xe, args->reserved[0] || args->reserved[1]))
		return false;

	return true;
}

static int xe_madvise_details_init(struct xe_vm *vm, const struct drm_xe_madvise *args,
				   struct xe_madvise_details *details)
{
	struct xe_device *xe = vm->xe;

	memset(details, 0, sizeof(*details));

	/* Store retained pointer for purgeable state */
	if (args->type == DRM_XE_VMA_ATTR_PURGEABLE_STATE) {
		details->retained_ptr = args->purge_state_val.retained_ptr;
		return 0;
	}

	if (args->type == DRM_XE_MEM_RANGE_ATTR_PREFERRED_LOC) {
		int fd = args->preferred_mem_loc.devmem_fd;
		struct drm_pagemap *dpagemap;

		if (fd <= 0)
			return 0;

		dpagemap = xe_drm_pagemap_from_fd(args->preferred_mem_loc.devmem_fd,
						  args->preferred_mem_loc.region_instance);
		if (XE_IOCTL_DBG(xe, IS_ERR(dpagemap)))
			return PTR_ERR(dpagemap);

		/* Don't allow a foreign placement without a fast interconnect! */
		if (XE_IOCTL_DBG(xe, dpagemap->pagemap->owner != vm->svm.peer.owner)) {
			drm_pagemap_put(dpagemap);
			return -ENOLINK;
		}
		details->dpagemap = dpagemap;
	}

	return 0;
}

static void xe_madvise_details_fini(struct xe_madvise_details *details)
{
	drm_pagemap_put(details->dpagemap);
}

static int xe_madvise_purgeable_retained_to_user(const struct xe_madvise_details *details)
{
	u32 retained;

	if (!details->retained_ptr)
		return 0;

	retained = !details->has_purged_bo;

	if (put_user(retained, (u32 __user *)u64_to_user_ptr(details->retained_ptr)))
		return -EFAULT;

	return 0;
}

static bool check_pat_args_are_sane(struct xe_device *xe,
				    struct xe_vmas_in_madvise_range *madvise_range,
				    u16 pat_index)
{
	u16 coh_mode = xe_pat_index_get_coh_mode(xe, pat_index);
	int i;

	/*
	 * Using coh_none with CPU cached buffers is not allowed on iGPU.
	 * On iGPU the GPU shares the LLC with the CPU, so with coh_none
	 * the GPU bypasses CPU caches and reads directly from DRAM,
	 * potentially seeing stale sensitive data from previously freed
	 * pages. On dGPU this restriction does not apply, because the
	 * platform does not provide a non-coherent system memory access
	 * path that would violate the DMA coherency contract.
	 */
	if (coh_mode != XE_COH_NONE || IS_DGFX(xe))
		return true;

	for (i = 0; i < madvise_range->num_vmas; i++) {
		struct xe_vma *vma = madvise_range->vmas[i];
		struct xe_bo *bo = xe_vma_bo(vma);

		if (bo) {
			/* BO with WB caching + COH_NONE is not allowed */
			if (XE_IOCTL_DBG(xe, bo->cpu_caching == DRM_XE_GEM_CPU_CACHING_WB))
				return false;
			/* Imported dma-buf without caching info, assume cached */
			if (XE_IOCTL_DBG(xe, !bo->cpu_caching))
				return false;
		} else if (XE_IOCTL_DBG(xe, xe_vma_is_cpu_addr_mirror(vma) ||
					    xe_vma_is_userptr(vma)))
			/* System memory (userptr/SVM) is always CPU cached */
			return false;
	}

	return true;
}

static bool check_bo_args_are_sane(struct xe_vm *vm, struct xe_vma **vmas,
				   int num_vmas, u32 atomic_val)
{
	struct xe_device *xe = vm->xe;
	struct xe_bo *bo;
	int i;

	for (i = 0; i < num_vmas; i++) {
		bo = xe_vma_bo(vmas[i]);
		if (!bo)
			continue;
		/*
		 * NOTE: The following atomic checks are platform-specific. For example,
		 * if a device supports CXL atomics, these may not be necessary or
		 * may behave differently.
		 */
		if (XE_IOCTL_DBG(xe, atomic_val == DRM_XE_ATOMIC_CPU &&
				 !(bo->flags & XE_BO_FLAG_SYSTEM)))
			return false;

		if (XE_IOCTL_DBG(xe, atomic_val == DRM_XE_ATOMIC_DEVICE &&
				 !(bo->flags & XE_BO_FLAG_VRAM0) &&
				 !(bo->flags & XE_BO_FLAG_VRAM1) &&
				 !(bo->flags & XE_BO_FLAG_SYSTEM &&
				   xe->info.has_device_atomics_on_smem)))
			return false;

		if (XE_IOCTL_DBG(xe, atomic_val == DRM_XE_ATOMIC_GLOBAL &&
				 (!(bo->flags & XE_BO_FLAG_SYSTEM) ||
				  (!(bo->flags & XE_BO_FLAG_VRAM0) &&
				   !(bo->flags & XE_BO_FLAG_VRAM1)))))
			return false;
	}
	return true;
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
	struct xe_vmas_in_madvise_range madvise_range = {
		/*
		 * Userspace may pass canonical (sign-extended) addresses.
		 * Strip the sign extension to get the internal non-canonical
		 * form used by the GPUVM, matching xe_vm_bind_ioctl() behavior.
		 */
		.addr = xe_device_uncanonicalize_addr(xe, args->start),
		.range = args->range,
	};
	struct xe_madvise_details details;
	u16 pat_index, coh_mode;
	struct xe_vm *vm;
	struct drm_exec exec;
	int err, attr_type;
	bool do_retained;

	vm = xe_vm_lookup(xef, args->vm_id);
	if (XE_IOCTL_DBG(xe, !vm))
		return -EINVAL;

	if (!madvise_args_are_sane(vm->xe, args)) {
		err = -EINVAL;
		goto put_vm;
	}

	/* Cache whether we need to write retained, and validate it's initialized to 0 */
	do_retained = args->type == DRM_XE_VMA_ATTR_PURGEABLE_STATE &&
		      args->purge_state_val.retained_ptr;
	if (do_retained) {
		u32 retained;
		u32 __user *retained_ptr;

		retained_ptr = u64_to_user_ptr(args->purge_state_val.retained_ptr);
		if (get_user(retained, retained_ptr)) {
			err = -EFAULT;
			goto put_vm;
		}

		if (XE_IOCTL_DBG(xe, retained != 0)) {
			err = -EINVAL;
			goto put_vm;
		}
	}

	xe_svm_flush(vm);

	err = down_write_killable(&vm->lock);
	if (err)
		goto put_vm;

	if (XE_IOCTL_DBG(xe, xe_vm_is_closed_or_banned(vm))) {
		err = -ENOENT;
		goto unlock_vm;
	}

	err = xe_madvise_details_init(vm, args, &details);
	if (err)
		goto unlock_vm;

	err = xe_vm_alloc_madvise_vma(vm, madvise_range.addr, args->range);
	if (err)
		goto madv_fini;

	err = get_vmas(vm, &madvise_range);
	if (err || !madvise_range.num_vmas)
		goto madv_fini;

	if (args->type == DRM_XE_MEM_RANGE_ATTR_PAT) {
		pat_index = array_index_nospec(args->pat_index.val, xe->pat.n_entries);
		coh_mode = xe_pat_index_get_coh_mode(xe, pat_index);
		if (XE_IOCTL_DBG(xe, madvise_range.has_svm_userptr_vmas &&
				 xe_device_is_l2_flush_optimized(xe) &&
				 (pat_index != 19 && coh_mode != XE_COH_2WAY))) {
			err = -EINVAL;
			goto madv_fini;
		}
	}

	if (args->type == DRM_XE_MEM_RANGE_ATTR_PAT) {
		if (!check_pat_args_are_sane(xe, &madvise_range,
					     args->pat_index.val)) {
			err = -EINVAL;
			goto free_vmas;
		}
	}

	if (madvise_range.has_bo_vmas) {
		if (args->type == DRM_XE_MEM_RANGE_ATTR_ATOMIC) {
			if (!check_bo_args_are_sane(vm, madvise_range.vmas,
						    madvise_range.num_vmas,
						    args->atomic.val)) {
				err = -EINVAL;
				goto free_vmas;
			}
		}

		drm_exec_init(&exec, DRM_EXEC_IGNORE_DUPLICATES | DRM_EXEC_INTERRUPTIBLE_WAIT, 0);
		drm_exec_until_all_locked(&exec) {
			for (int i = 0; i < madvise_range.num_vmas; i++) {
				struct xe_bo *bo = xe_vma_bo(madvise_range.vmas[i]);

				if (!bo)
					continue;

				if (args->type == DRM_XE_MEM_RANGE_ATTR_PAT) {
					if (XE_IOCTL_DBG(xe, bo->ttm.base.import_attach &&
							 xe_device_is_l2_flush_optimized(xe) &&
							 (pat_index != 19 &&
							  coh_mode != XE_COH_2WAY))) {
						err = -EINVAL;
						goto err_fini;
					}
				}

				err = drm_exec_lock_obj(&exec, &bo->ttm.base);
				drm_exec_retry_on_contention(&exec);
				if (err)
					goto err_fini;
			}
		}
	}

	if (madvise_range.has_svm_userptr_vmas) {
		err = xe_svm_notifier_lock_interruptible(vm);
		if (err)
			goto err_fini;
	}

	attr_type = array_index_nospec(args->type, ARRAY_SIZE(madvise_funcs));

	/* Ensure the madvise function exists for this type */
	if (!madvise_funcs[attr_type]) {
		err = -EINVAL;
		goto err_fini;
	}

	madvise_funcs[attr_type](xe, vm, madvise_range.vmas, madvise_range.num_vmas, args,
				 &details);

	err = xe_vm_invalidate_madvise_range(vm, madvise_range.addr,
					     madvise_range.addr + args->range);

	if (madvise_range.has_svm_userptr_vmas)
		xe_svm_notifier_unlock(vm);

err_fini:
	if (madvise_range.has_bo_vmas)
		drm_exec_fini(&exec);
free_vmas:
	kfree(madvise_range.vmas);
	madvise_range.vmas = NULL;
madv_fini:
	xe_madvise_details_fini(&details);
unlock_vm:
	up_write(&vm->lock);

	/* Write retained value to user after releasing all locks */
	if (!err && do_retained)
		err = xe_madvise_purgeable_retained_to_user(&details);
put_vm:
	xe_vm_put(vm);
	return err;
}

/**
 * xe_vma_reset_to_default_attrs - Reset madvise attrs to defaults
 * @vma: VMA to reset
 */
static void xe_vma_reset_to_default_attrs(struct xe_vma *vma)
{
	struct xe_vma_mem_attr default_attr = {
		.preferred_loc.devmem_fd = DRM_XE_PREFERRED_LOC_DEFAULT_DEVICE,
		.preferred_loc.migration_policy = DRM_XE_MIGRATE_ALL_PAGES,
		.default_pat_index = vma->attr.default_pat_index,
		.pat_index = vma->attr.default_pat_index,
		.atomic_access = DRM_XE_ATOMIC_UNDEFINED,
		.purgeable_state = XE_MADV_PURGEABLE_WILLNEED,
	};

	xe_vma_mem_attr_copy(&vma->attr, &default_attr);
}

/**
 * xe_vm_madvise_process_unmap - Reset attrs for a GPUVA range
 * @vm: VM
 * @start: start of range
 * @end: end of range
 *
 * Process CPU-only VMAs overlapping [@start, @end).
 *
 * Return: 0 on success, negative error otherwise.
 */
static int xe_vm_madvise_process_unmap(struct xe_vm *vm, u64 start, u64 end)
{
	u64 addr = start;
	int err;

	lockdep_assert_held_write(&vm->lock);

	if (xe_vm_is_closed_or_banned(vm))
		return 0;

	while (addr < end) {
		struct xe_vma *vma;
		u64 seg_start, seg_end;
		bool has_default_attr;

		vma = xe_vm_find_overlapping_vma(vm, addr, end - addr);
		if (!vma)
			break;

		/* GPU-touched VMAs are handled by SVM. */
		if (!xe_vma_has_cpu_autoreset_active(vma)) {
			addr = xe_vma_end(vma);
			continue;
		}

		has_default_attr = xe_vma_has_default_mem_attrs(vma);
		seg_start = max(addr, xe_vma_start(vma));
		seg_end = min(end, xe_vma_end(vma));

		/* Merge adjacent default-attr VMAs when possible. */
		if (has_default_attr &&
		    xe_vma_start(vma) >= start &&
		    xe_vma_end(vma) <= end) {
			seg_start = xe_vma_start(vma);
			seg_end = xe_vma_end(vma);
			xe_vm_find_cpu_addr_mirror_vma_range(vm, &seg_start, &seg_end);
			if (xe_vma_start(vma) == seg_start && xe_vma_end(vma) == seg_end) {
				/* Nothing to merge. */
				addr = seg_end;
				continue;
			}
		} else if (xe_vma_start(vma) == seg_start && xe_vma_end(vma) == seg_end) {
			/* Exact VMA match, reset in place. */
			xe_vma_reset_to_default_attrs(vma);
			addr = seg_end;
			continue;
		}

		err = xe_vm_alloc_cpu_addr_mirror_vma(vm, seg_start, seg_end - seg_start);
		if (err) {
			if (err == -ENOENT) {
				/* VMA was removed before the worker ran. */
				addr = seg_end;
				continue;
			}
			return err;
		}

		addr = seg_end;
	}

	return 0;
}

/**
 * xe_vm_madvise_process_unmap_holes - Reset attrs for CPU holes
 * @vm: VM
 * @mm: mm backing the CPU mirror
 * @start: start of the pending interval
 * @end: end of the pending interval
 *
 * Walk [@start, @end) and process only ranges not covered by a CPU VMA.
 * Mapped ranges are skipped so partial-unmap siblings keep their attrs.
 *
 * Caller must hold vm->lock for write and mmap_read_lock(@mm).
 *
 * Return: 0 on success, negative error otherwise.
 */
static int xe_vm_madvise_process_unmap_holes(struct xe_vm *vm,
					     struct mm_struct *mm,
					     u64 start, u64 end)
{
	u64 addr = start;

	lockdep_assert_held_write(&vm->lock);
	mmap_assert_locked(mm);

	while (addr < end) {
		struct vm_area_struct *cpu_vma;
		u64 hole_start, hole_end;
		int err;

		cpu_vma = find_vma(mm, addr);

		if (cpu_vma && cpu_vma->vm_start <= addr) {
			addr = min_t(u64, cpu_vma->vm_end, end);
			continue;
		}

		hole_start = addr;
		hole_end = cpu_vma ? min_t(u64, cpu_vma->vm_start, end) : end;

		err = xe_vm_madvise_process_unmap(vm, hole_start, hole_end);
		if (err)
			return err;

		addr = hole_end;
	}

	return 0;
}

/**
 * xe_madvise_work_func - Worker to process pending unmap events
 * @w: work_struct embedded in xe_madvise_notifier
 *
 * Drains pending intervals recorded by the callback. The worker loops so
 * events queued while it is running are not lost.
 */
static void xe_madvise_work_func(struct work_struct *w)
{
	struct xe_madvise_notifier *notifier =
		container_of(w, struct xe_madvise_notifier, work);
	struct xe_vm *vm = notifier->vm;

	for (;;) {
		struct mm_struct *mm;
		u64 start, end;
		int err;

		spin_lock(&notifier->work_lock);
		if (!notifier->work_pending) {
			spin_unlock(&notifier->work_lock);
			break;
		}
		start = notifier->work_start;
		end = notifier->work_end;
		notifier->work_pending = false;
		spin_unlock(&notifier->work_lock);

		/* The mm is going away, teardown will clean up. */
		mm = vm->svm.gpusvm.mm;
		if (!mm || !mmget_not_zero(mm))
			break;

		down_write(&vm->lock);
		mmap_read_lock(mm);

		err = xe_vm_madvise_process_unmap_holes(vm, mm, start, end);

		mmap_read_unlock(mm);
		up_write(&vm->lock);
		mmput(mm);

		if (err)
			drm_warn(&vm->xe->drm,
				 "madvise autoreset failed [%#llx-%#llx]: %d\n",
				 start, end, err);
	}
}

/**
 * xe_madvise_notifier_callback - MMU notifier callback for CPU munmap
 * @mni: mmu_interval_notifier
 * @range: mmu_notifier_range
 * @cur_seq: current sequence number
 *
 * Records one pending interval without allocating. Later events widen it.
 * The worker checks the CPU mm before resetting attributes.
 *
 * Return: false for non-blockable invalidations, true otherwise.
 */
static bool xe_madvise_notifier_callback(struct mmu_interval_notifier *mni,
					 const struct mmu_notifier_range *range,
					 unsigned long cur_seq)
{
	struct xe_madvise_notifier *notifier =
		container_of(mni, struct xe_madvise_notifier, mmu_notifier);
	struct xe_vm *vm = notifier->vm;
	u64 adj_start, adj_end;

	if (range->event != MMU_NOTIFY_UNMAP)
		return true;

	if (!mmu_notifier_range_blockable(range))
		return false;

	if (xe_vm_is_closed(vm))
		return true;

	mmu_interval_set_seq(mni, cur_seq);

	/* Clamp to notifier boundaries and ignore non-overlap. */
	adj_start = max_t(u64, range->start, notifier->vma_start);
	adj_end = min_t(u64, range->end, notifier->vma_end);

	if (adj_start >= adj_end)
		return true;

	/* Bail if teardown started; trylock fails once fini holds write. */
	if (!down_read_trylock(&vm->svm.madvise_work.teardown_rwsem))
		return true;

	/* fini may have NULLed wq before we got here; check under read lock. */
	if (!vm->svm.madvise_work.wq)
		goto out;

	spin_lock(&notifier->work_lock);
	if (notifier->work_pending) {
		/*
		 * Widen pending work. The worker only resets CPU holes,
		 * so mapped siblings are left untouched.
		 */
		notifier->work_start = min(notifier->work_start, adj_start);
		notifier->work_end = max(notifier->work_end, adj_end);
	} else {
		notifier->work_start = adj_start;
		notifier->work_end = adj_end;
		notifier->work_pending = true;
	}
	spin_unlock(&notifier->work_lock);

	queue_work(vm->svm.madvise_work.wq, &notifier->work);

out:
	up_read(&vm->svm.madvise_work.teardown_rwsem);
	return true;
}

static const struct mmu_interval_notifier_ops xe_madvise_notifier_ops = {
	.invalidate = xe_madvise_notifier_callback,
};

/**
 * xe_vm_madvise_init - Initialize madvise notifier infrastructure
 * @vm: VM
 *
 * Sets up workqueue for async munmap processing.
 *
 * Return: 0 on success, -ENOMEM on failure
 */
int xe_vm_madvise_init(struct xe_vm *vm)
{
	/* Already initialized. */
	if (vm->svm.madvise_work.wq)
		return 0;

	mt_init(&vm->svm.madvise_notifiers);
	INIT_LIST_HEAD(&vm->svm.madvise_notifier_list);

	/* Separate class for notifier teardown. */
	__init_rwsem(&vm->svm.madvise_work.teardown_rwsem,
		     "xe_madvise_teardown", &xe_madvise_teardown_key);

	/* Not used from reclaim paths. */
	vm->svm.madvise_work.wq = alloc_workqueue("xe_madvise", WQ_UNBOUND, 0);
	if (!vm->svm.madvise_work.wq) {
		mtree_destroy(&vm->svm.madvise_notifiers);
		return -ENOMEM;
	}

	return 0;
}

static void xe_madvise_notifier_free(struct xe_madvise_notifier *notifier)
{
	xe_vm_put(notifier->vm);
	kfree(notifier);
}

static void xe_madvise_notifier_remove_and_free(struct xe_madvise_notifier *notifier)
{
	mmu_interval_notifier_remove(&notifier->mmu_notifier);
	cancel_work_sync(&notifier->work);
	xe_madvise_notifier_free(notifier);
}

static struct xe_madvise_notifier *
xe_madvise_notifier_alloc(struct xe_vm *vm, u64 start, u64 end)
{
	struct xe_madvise_notifier *notifier;

	notifier = kzalloc_obj(*notifier, GFP_KERNEL);
	if (!notifier)
		return NULL;

	notifier->vm = xe_vm_get(vm);
	notifier->vma_start = start;
	notifier->vma_end = end;
	INIT_LIST_HEAD(&notifier->link);
	spin_lock_init(&notifier->work_lock);
	notifier->work_pending = false;
	INIT_WORK(&notifier->work, xe_madvise_work_func);

	return notifier;
}

static bool xe_madvise_notifier_exact(const struct xe_madvise_notifier *notifier,
				      u64 start, u64 end)
{
	return notifier->vma_start == start && notifier->vma_end == end;
}

static bool xe_madvise_notifier_fully_covered(const struct xe_madvise_notifier *notifier,
					      u64 start, u64 end)
{
	/*
	 * Broader notifiers may still cover split siblings, so only remove
	 * notifiers fully covered by the new range.
	 */
	return notifier->vma_start >= start && notifier->vma_end <= end;
}

/**
 * xe_vm_madvise_fini - Cleanup all madvise notifiers
 * @vm: VM
 *
 * Tears down notifiers and drains workqueue. Safe if init partially failed.
 */
void xe_vm_madvise_fini(struct xe_vm *vm)
{
	struct xe_madvise_notifier *notifier, *next;
	struct workqueue_struct *wq;
	LIST_HEAD(tmp);

	/* Nothing to do if init never ran. */
	if (!vm->svm.madvise_work.wq)
		return;

	/* Block new callbacks. */
	down_write(&vm->svm.madvise_work.teardown_rwsem);

	/* Stage all owned notifiers from the VM list. */
	list_for_each_entry_safe(notifier, next,
				 &vm->svm.madvise_notifier_list, link) {
		list_del_init(&notifier->link);
		list_add_tail(&notifier->link, &tmp);
	}

	/* VM is closed; safe to destroy the tree. */
	mtree_destroy(&vm->svm.madvise_notifiers);

	/* NULL wq so late callbacks bail. */
	wq = vm->svm.madvise_work.wq;
	vm->svm.madvise_work.wq = NULL;

	up_write(&vm->svm.madvise_work.teardown_rwsem);

	/*
	 * Remove notifiers outside rwsem; remove() may block on mmap_lock.
	 */
	list_for_each_entry(notifier, &tmp, link)
		mmu_interval_notifier_remove(&notifier->mmu_notifier);

	/* Drain work before freeing; workers reference notifier via container_of. */
	if (wq) {
		drain_workqueue(wq);
		destroy_workqueue(wq);
	}

	/* Safe to free now: no callbacks can fire, no workers are running. */
	list_for_each_entry_safe(notifier, next, &tmp, link) {
		list_del(&notifier->link);
		xe_madvise_notifier_free(notifier);
	}
}

/**
 * xe_vm_madvise_register_notifier_range - Register MMU notifier for address range
 * @vm: VM
 * @start: Start address (page-aligned)
 * @end: End address (page-aligned)
 *
 * Registers interval notifier for munmap tracking. Uses addresses (not VMA pointers)
 * to avoid UAF after dropping vm->lock. Deduplicates by range.
 *
 * Return: 0 on success, negative error code on failure
 */
int xe_vm_madvise_register_notifier_range(struct xe_vm *vm, u64 start, u64 end)
{
	struct xe_madvise_notifier *notifier, *old, *tmp;
	struct xe_madvise_notifier *existing;
	LIST_HEAD(displaced);
	int err;

	if (!IS_ALIGNED(start, PAGE_SIZE) || !IS_ALIGNED(end, PAGE_SIZE))
		return -EINVAL;

	if (WARN_ON_ONCE(end <= start))
		return -EINVAL;

	if (!vm->svm.gpusvm.mm)
		return -EINVAL;

	notifier = xe_madvise_notifier_alloc(vm, start, end);
	if (!notifier)
		return -ENOMEM;

	/* Insert before vm->lock, this may take mmap_lock. */
	err = mmu_interval_notifier_insert(&notifier->mmu_notifier,
					   vm->svm.gpusvm.mm,
					   start, end - start,
					   &xe_madvise_notifier_ops);
	if (err) {
		xe_madvise_notifier_free(notifier);
		return err;
	}

	/* Dedup and store under vm->lock. */
	down_write(&vm->lock);

	if (xe_vm_is_closed_or_banned(vm)) {
		err = -ENOENT;
		goto unlock_remove_new;
	}

	/* Dedup by stored range; tree slots can be fragmented by partial overlap. */
	list_for_each_entry(existing, &vm->svm.madvise_notifier_list, link) {
		if (xe_madvise_notifier_exact(existing, start, end)) {
			err = 0;
			goto unlock_remove_new;
		}
	}

	/*
	 * Store first. The VM list owns notifier lifetime, so there is
	 * nothing to restore on failure.
	 */
	err = mtree_store_range(&vm->svm.madvise_notifiers, start, end - 1,
				notifier, GFP_KERNEL);
	if (err)
		goto unlock_remove_new;

	/* Keep the new notifier reachable for teardown. */
	list_add_tail(&notifier->link, &vm->svm.madvise_notifier_list);

	/*
	 * Drop fully covered old notifiers. Broader notifiers may still cover
	 * split siblings, so leave them alive.
	 */
	list_for_each_entry_safe(old, tmp, &vm->svm.madvise_notifier_list, link) {
		if (old == notifier)
			continue;
		if (!xe_madvise_notifier_fully_covered(old, start, end))
			continue;

		list_del_init(&old->link);
		list_add(&old->link, &displaced);
	}

	up_write(&vm->lock);

	/*
	 * Remove outside vm->lock. remove_and_free() drains callbacks and
	 * work before freeing the notifier.
	 */
	list_for_each_entry_safe(old, tmp, &displaced, link) {
		list_del_init(&old->link);
		xe_madvise_notifier_remove_and_free(old);
	}

	return 0;

unlock_remove_new:
	up_write(&vm->lock);
	xe_madvise_notifier_remove_and_free(notifier);

	return err;
}
