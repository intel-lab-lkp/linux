/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2016 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#ifndef _MSM_GEM_VMA_H_
#define _MSM_GEM_VMA_H_

#define vm_dbg(fmt, ...) pr_debug("%s:%d: "fmt"\n", __func__, __LINE__, ##__VA_ARGS__)

/**
 * struct msm_vm_map_op - create new pgtable mapping
 */
struct msm_vm_map_op {
	/** @iova: start address for mapping */
	uint64_t iova;
	/** @range: size of the region to map */
	uint64_t range;
	/** @offset: offset into @sgt to map */
	uint64_t offset;
	/** @sgt: pages to map, or NULL for a PRR mapping */
	struct sg_table *sgt;
	/** @prot: the mapping protection flags */
	int prot;

	/**
	 * @queue_id: The id of the submitqueue the operation is performed
	 * on, or zero for (in particular) UNMAP ops triggered outside of
	 * a submitqueue (ie. process cleanup)
	 */
	int queue_id;
};

/**
 * struct msm_vm_unmap_op - unmap a range of pages from pgtable
 */
struct msm_vm_unmap_op {
	/** @iova: start address for unmap */
	uint64_t iova;
	/** @range: size of region to unmap */
	uint64_t range;

	/** @reason: The reason for the unmap */
	const char *reason;

	/**
	 * @queue_id: The id of the submitqueue the operation is performed
	 * on, or zero for (in particular) UNMAP ops triggered outside of
	 * a submitqueue (ie. process cleanup)
	 */
	int queue_id;
};

static void
vm_log(struct msm_gem_vm *vm, const char *op, uint64_t iova, uint64_t range, int queue_id)
{
	int idx;

	if (!vm->managed)
		lockdep_assert_held(&vm->mmu_lock);

	vm_dbg("%s:%p:%d: %016llx %016llx", op, vm, queue_id, iova, iova + range);

	if (!vm->log)
		return;

	idx = vm->log_idx;
	vm->log[idx].op = op;
	vm->log[idx].iova = iova;
	vm->log[idx].range = range;
	vm->log[idx].queue_id = queue_id;
	vm->log_idx = (vm->log_idx + 1) & ((1 << vm->log_shift) - 1);
}

static void
vm_unmap_op(struct msm_gem_vm *vm, const struct msm_vm_unmap_op *op)
{
	const char *reason = op->reason;

	if (!reason)
		reason = "unmap";

	vm_log(vm, reason, op->iova, op->range, op->queue_id);

	vm->mmu->funcs->unmap(vm->mmu, op->iova, op->range);
}

static int
vm_map_op(struct msm_gem_vm *vm, const struct msm_vm_map_op *op)
{
	vm_log(vm, "map", op->iova, op->range, op->queue_id);

	return vm->mmu->funcs->map(vm->mmu, op->iova, op->sgt, op->offset,
				   op->range, op->prot);
}

#ifdef CONFIG_DRM_MSM_ADRENO
int msm_gem_vm_sm_step_map(struct drm_gpuva_op *op, void *_arg);
int msm_gem_vm_sm_step_remap(struct drm_gpuva_op *op, void *arg);
int msm_gem_vm_sm_step_unmap(struct drm_gpuva_op *op, void *_arg);

int msm_gem_vm_sched_init(struct msm_gem_vm *vm, struct drm_device *drm);
void msm_gem_vm_sched_fini(struct msm_gem_vm *vm);
#else

#define msm_gem_vm_sm_step_map   NULL
#define msm_gem_vm_sm_step_remap NULL
#define msm_gem_vm_sm_step_unmap NULL

static inline int msm_gem_vm_sched_init(struct msm_gem_vm *vm, struct drm_device *drm)
{
	return -EINVAL;
}

static inline void msm_gem_vm_sched_fini(struct msm_gem_vm *vm) {}
#endif

#endif
