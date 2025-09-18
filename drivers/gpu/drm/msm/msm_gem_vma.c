// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2016 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include "drm/drm_file.h"
#include "drm/msm_drm.h"
#include "linux/file.h"
#include "linux/sync_file.h"

#include "msm_drv.h"
#include "msm_gem.h"
#include "msm_gem_vma.h"
#include "msm_gpu.h"
#include "msm_mmu.h"
#include "msm_syncobj.h"

static uint vm_log_shift = 0;
MODULE_PARM_DESC(vm_log_shift, "Length of VM op log");
module_param_named(vm_log_shift, vm_log_shift, uint, 0600);

static void
msm_gem_vm_free(struct drm_gpuvm *gpuvm)
{
	struct msm_gem_vm *vm = container_of(gpuvm, struct msm_gem_vm, base);

	drm_mm_takedown(&vm->mm);
	if (vm->mmu)
		vm->mmu->funcs->destroy(vm->mmu);
	dma_fence_put(vm->last_fence);
	put_pid(vm->pid);
	kfree(vm->log);
	kfree(vm);
}

/**
 * msm_gem_vm_unusable() - Mark a VM as unusable
 * @gpuvm: the VM to mark unusable
 */
void
msm_gem_vm_unusable(struct drm_gpuvm *gpuvm)
{
	struct msm_gem_vm *vm = to_msm_vm(gpuvm);
	uint32_t vm_log_len = (1 << vm->log_shift);
	uint32_t vm_log_mask = vm_log_len - 1;
	uint32_t nr_vm_logs;
	int first;

	vm->unusable = true;

	/* Bail if no log, or empty log: */
	if (!vm->log || !vm->log[0].op)
		return;

	mutex_lock(&vm->mmu_lock);

	/*
	 * log_idx is the next entry to overwrite, meaning it is the oldest, or
	 * first, entry (other than the special case handled below where the
	 * log hasn't wrapped around yet)
	 */
	first = vm->log_idx;

	if (!vm->log[first].op) {
		/*
		 * If the next log entry has not been written yet, then only
		 * entries 0 to idx-1 are valid (ie. we haven't wrapped around
		 * yet)
		 */
		nr_vm_logs = MAX(0, first - 1);
		first = 0;
	} else {
		nr_vm_logs = vm_log_len;
	}

	pr_err("vm-log:\n");
	for (int i = 0; i < nr_vm_logs; i++) {
		int idx = (i + first) & vm_log_mask;
		struct msm_gem_vm_log_entry *e = &vm->log[idx];
		pr_err("  - %s:%d: 0x%016llx-0x%016llx\n",
		       e->op, e->queue_id, e->iova,
		       e->iova + e->range);
	}

	mutex_unlock(&vm->mmu_lock);
}

/* Actually unmap memory for the vma */
void msm_gem_vma_unmap(struct drm_gpuva *vma, const char *reason)
{
	struct msm_gem_vm *vm = to_msm_vm(vma->vm);
	struct msm_gem_vma *msm_vma = to_msm_vma(vma);

	/* Don't do anything if the memory isn't mapped */
	if (!msm_vma->mapped)
		return;

	/*
	 * The mmu_lock is only needed when preallocation is used.  But
	 * in that case we don't need to worry about recursion into
	 * shrinker
	 */
	if (!vm->managed)
		 mutex_lock(&vm->mmu_lock);

	vm_unmap_op(vm, &(struct msm_vm_unmap_op){
		.iova = vma->va.addr,
		.range = vma->va.range,
		.reason = reason,
	});

	if (!vm->managed)
		mutex_unlock(&vm->mmu_lock);

	msm_vma->mapped = false;
}

/* Map and pin vma: */
int
msm_gem_vma_map(struct drm_gpuva *vma, int prot, struct sg_table *sgt)
{
	struct msm_gem_vm *vm = to_msm_vm(vma->vm);
	struct msm_gem_vma *msm_vma = to_msm_vma(vma);
	int ret;

	if (GEM_WARN_ON(!vma->va.addr))
		return -EINVAL;

	if (msm_vma->mapped)
		return 0;

	msm_vma->mapped = true;

	/*
	 * The mmu_lock is only needed when preallocation is used.  But
	 * in that case we don't need to worry about recursion into
	 * shrinker
	 */
	if (!vm->managed)
		mutex_lock(&vm->mmu_lock);

	/*
	 * NOTE: if not using pgtable preallocation, we cannot hold
	 * a lock across map/unmap which is also used in the job_run()
	 * path, as this can cause deadlock in job_run() vs shrinker/
	 * reclaim.
	 */
	ret = vm_map_op(vm, &(struct msm_vm_map_op){
		.iova = vma->va.addr,
		.range = vma->va.range,
		.offset = vma->gem.offset,
		.sgt = sgt,
		.prot = prot,
	});

	if (!vm->managed)
		mutex_unlock(&vm->mmu_lock);

	if (ret)
		msm_vma->mapped = false;

	return ret;
}

/* Close an iova.  Warn if it is still in use */
void msm_gem_vma_close(struct drm_gpuva *vma)
{
	struct msm_gem_vm *vm = to_msm_vm(vma->vm);
	struct msm_gem_vma *msm_vma = to_msm_vma(vma);

	GEM_WARN_ON(msm_vma->mapped);

	drm_gpuvm_resv_assert_held(&vm->base);

	if (vma->gem.obj)
		msm_gem_assert_locked(vma->gem.obj);

	if (vma->va.addr && vm->managed)
		drm_mm_remove_node(&msm_vma->node);

	drm_gpuva_remove(vma);
	drm_gpuva_unlink(vma);

	kfree(vma);
}

/* Create a new vma and allocate an iova for it */
struct drm_gpuva *
msm_gem_vma_new(struct drm_gpuvm *gpuvm, struct drm_gem_object *obj,
		u64 offset, u64 range_start, u64 range_end)
{
	struct msm_gem_vm *vm = to_msm_vm(gpuvm);
	struct drm_gpuvm_bo *vm_bo;
	struct msm_gem_vma *vma;
	int ret;

	drm_gpuvm_resv_assert_held(&vm->base);

	vma = kzalloc(sizeof(*vma), GFP_KERNEL);
	if (!vma)
		return ERR_PTR(-ENOMEM);

	if (vm->managed) {
		BUG_ON(offset != 0);
		BUG_ON(!obj);  /* NULL mappings not valid for kernel managed VM */
		ret = drm_mm_insert_node_in_range(&vm->mm, &vma->node,
						obj->size, PAGE_SIZE, 0,
						range_start, range_end, 0);

		if (ret)
			goto err_free_vma;

		range_start = vma->node.start;
		range_end   = range_start + obj->size;
	}

	if (obj)
		GEM_WARN_ON((range_end - range_start) > obj->size);

	struct drm_gpuva_op_map op_map = {
		.va.addr = range_start,
		.va.range = range_end - range_start,
		.gem.obj = obj,
		.gem.offset = offset,
	};

	drm_gpuva_init_from_op(&vma->base, &op_map);
	vma->mapped = false;

	ret = drm_gpuva_insert(&vm->base, &vma->base);
	if (ret)
		goto err_free_range;

	if (!obj)
		return &vma->base;

	vm_bo = drm_gpuvm_bo_obtain(&vm->base, obj);
	if (IS_ERR(vm_bo)) {
		ret = PTR_ERR(vm_bo);
		goto err_va_remove;
	}

	drm_gpuvm_bo_extobj_add(vm_bo);
	drm_gpuva_link(&vma->base, vm_bo);
	GEM_WARN_ON(drm_gpuvm_bo_put(vm_bo));

	return &vma->base;

err_va_remove:
	drm_gpuva_remove(&vma->base);
err_free_range:
	if (vm->managed)
		drm_mm_remove_node(&vma->node);
err_free_vma:
	kfree(vma);
	return ERR_PTR(ret);
}

static int
msm_gem_vm_bo_validate(struct drm_gpuvm_bo *vm_bo, struct drm_exec *exec)
{
	struct drm_gem_object *obj = vm_bo->obj;
	struct drm_gpuva *vma;
	int ret;

	vm_dbg("validate: %p", obj);

	msm_gem_assert_locked(obj);

	drm_gpuvm_bo_for_each_va (vma, vm_bo) {
		ret = msm_gem_pin_vma_locked(obj, vma);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct drm_gpuvm_ops msm_gpuvm_ops = {
	.vm_free = msm_gem_vm_free,
	.vm_bo_validate = msm_gem_vm_bo_validate,
	.sm_step_map = msm_gem_vm_sm_step_map,
	.sm_step_remap = msm_gem_vm_sm_step_remap,
	.sm_step_unmap = msm_gem_vm_sm_step_unmap,
};

/**
 * msm_gem_vm_create() - Create and initialize a &msm_gem_vm
 * @drm: the drm device
 * @mmu: the backing MMU objects handling mapping/unmapping
 * @name: the name of the VM
 * @va_start: the start offset of the VA space
 * @va_size: the size of the VA space
 * @managed: is it a kernel managed VM?
 *
 * In a kernel managed VM, the kernel handles address allocation, and only
 * synchronous operations are supported.  In a user managed VM, userspace
 * handles virtual address allocation, and both async and sync operations
 * are supported.
 */
struct drm_gpuvm *
msm_gem_vm_create(struct drm_device *drm, struct msm_mmu *mmu, const char *name,
		  u64 va_start, u64 va_size, bool managed)
{
	/*
	 * We mostly want to use DRM_GPUVM_RESV_PROTECTED, except that
	 * makes drm_gpuvm_bo_evict() a no-op for extobjs (ie. we loose
	 * tracking that an extobj is evicted) :facepalm:
	 */
	enum drm_gpuvm_flags flags = 0;
	struct msm_gem_vm *vm;
	struct drm_gem_object *dummy_gem;
	int ret = 0;

	if (IS_ERR(mmu))
		return ERR_CAST(mmu);

	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return ERR_PTR(-ENOMEM);

	dummy_gem = drm_gpuvm_resv_object_alloc(drm);
	if (!dummy_gem) {
		ret = -ENOMEM;
		goto err_free_vm;
	}

	if (!managed) {
		ret = msm_gem_vm_sched_init(vm, drm);
		if (ret)
			goto err_free_dummy;
	}

	drm_gpuvm_init(&vm->base, name, flags, drm, dummy_gem,
		       va_start, va_size, 0, 0, &msm_gpuvm_ops);
	drm_gem_object_put(dummy_gem);

	vm->mmu = mmu;
	mutex_init(&vm->mmu_lock);
	vm->managed = managed;

	drm_mm_init(&vm->mm, va_start, va_size);

	/*
	 * We don't really need vm log for kernel managed VMs, as the kernel
	 * is responsible for ensuring that GEM objs are mapped if they are
	 * used by a submit.  Furthermore we piggyback on mmu_lock to serialize
	 * access to the log.
	 *
	 * Limit the max log_shift to 8 to prevent userspace from asking us
	 * for an unreasonable log size.
	 */
	if (!managed)
		vm->log_shift = MIN(vm_log_shift, 8);

	if (vm->log_shift) {
		vm->log = kmalloc_array(1 << vm->log_shift, sizeof(vm->log[0]),
					GFP_KERNEL | __GFP_ZERO);
	}

	return &vm->base;

err_free_dummy:
	drm_gem_object_put(dummy_gem);

err_free_vm:
	kfree(vm);
	return ERR_PTR(ret);
}

/**
 * msm_gem_vm_close() - Close a VM
 * @gpuvm: The VM to close
 *
 * Called when the drm device file is closed, to tear down VM related resources
 * (which will drop refcounts to GEM objects that were still mapped into the
 * VM at the time).
 */
void
msm_gem_vm_close(struct drm_gpuvm *gpuvm)
{
	struct msm_gem_vm *vm = to_msm_vm(gpuvm);
	struct drm_gpuva *vma, *tmp;
	struct drm_exec exec;

	/*
	 * For kernel managed VMs, the VMAs are torn down when the handle is
	 * closed, so nothing more to do.
	 */
	if (vm->managed)
		return;

	if (vm->last_fence)
		dma_fence_wait(vm->last_fence, false);

	msm_gem_vm_sched_fini(vm);

	/* Tear down any remaining mappings: */
	drm_exec_init(&exec, 0, 2);
	drm_exec_until_all_locked (&exec) {
		drm_exec_lock_obj(&exec, drm_gpuvm_resv_obj(gpuvm));
		drm_exec_retry_on_contention(&exec);

		drm_gpuvm_for_each_va_safe (vma, tmp, gpuvm) {
			struct drm_gem_object *obj = vma->gem.obj;

			/*
			 * MSM_BO_NO_SHARE objects share the same resv as the
			 * VM, in which case the obj is already locked:
			 */
			if (obj && (obj->resv == drm_gpuvm_resv(gpuvm)))
				obj = NULL;

			if (obj) {
				drm_exec_lock_obj(&exec, obj);
				drm_exec_retry_on_contention(&exec);
			}

			msm_gem_vma_unmap(vma, "close");
			msm_gem_vma_close(vma);

			if (obj) {
				drm_exec_unlock_obj(&exec, obj);
			}
		}
	}
	drm_exec_fini(&exec);
}
