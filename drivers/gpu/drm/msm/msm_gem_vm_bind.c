// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2016 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <drm/drm_file.h>
#include <drm/msm_drm.h>

#include <linux/file.h>
#include <linux/sync_file.h>

#include "msm_drv.h"
#include "msm_gem.h"
#include "msm_gem_vma.h"
#include "msm_gpu.h"
#include "msm_mmu.h"
#include "msm_syncobj.h"

/**
 * struct msm_vma_op - A MAP or UNMAP operation
 */
struct msm_vm_op {
	/** @op: The operation type */
	enum {
		MSM_VM_OP_MAP = 1,
		MSM_VM_OP_UNMAP,
	} op;
	union {
		/** @map: Parameters used if op == MSM_VMA_OP_MAP */
		struct msm_vm_map_op map;
		/** @unmap: Parameters used if op == MSM_VMA_OP_UNMAP */
		struct msm_vm_unmap_op unmap;
	};
	/** @node: list head in msm_vm_bind_job::vm_ops */
	struct list_head node;

	/**
	 * @obj: backing object for pages to be mapped/unmapped
	 *
	 * Async unmap ops, in particular, must hold a reference to the
	 * original GEM object backing the mapping that will be unmapped.
	 * But the same can be required in the map path, for example if
	 * there is not a corresponding unmap op, such as process exit.
	 *
	 * This ensures that the pages backing the mapping are not freed
	 * before the mapping is torn down.
	 */
	struct drm_gem_object *obj;
};

/**
 * struct msm_vm_bind_job - Tracking for a VM_BIND ioctl
 *
 * A table of userspace requested VM updates (MSM_VM_BIND_OP_UNMAP/MAP/MAP_NULL)
 * gets applied to the vm, generating a list of VM ops (MSM_VM_OP_MAP/UNMAP)
 * which are applied to the pgtables asynchronously.  For example a userspace
 * requested MSM_VM_BIND_OP_MAP could end up generating both an MSM_VM_OP_UNMAP
 * to unmap an existing mapping, and a MSM_VM_OP_MAP to apply the new mapping.
 */
struct msm_vm_bind_job {
	/** @base: base class for drm_sched jobs */
	struct drm_sched_job base;
	/** @fence: The fence that is signaled when job completes */
	struct dma_fence *fence;
	/** @vm: The VM being operated on */
	struct drm_gpuvm *vm;
	/** @queue: The queue that the job runs on */
	struct msm_gpu_submitqueue *queue;
	/** @prealloc: Tracking for pre-allocated MMU pgtable pages */
	struct msm_mmu_prealloc prealloc;
	/** @vm_ops: a list of struct msm_vm_op */
	struct list_head vm_ops;
	/** @bos_pinned: are the GEM objects being bound pinned? */
	bool bos_pinned;
	/** @nr_ops: the number of userspace requested ops */
	unsigned int nr_ops;
	/**
	 * @ops: the userspace requested ops
	 *
	 * The userspace requested ops are copied/parsed and validated
	 * before we start applying the updates to try to do as much up-
	 * front error checking as possible, to avoid the VM being in an
	 * undefined state due to partially executed VM_BIND.
	 *
	 * This table also serves to hold a reference to the backing GEM
	 * objects.
	 */
	struct msm_vm_bind_op {
		uint32_t op;
		uint32_t flags;
		union {
			struct drm_gem_object *obj;
			uint32_t handle;
		};
		uint64_t obj_offset;
		uint64_t iova;
		uint64_t range;
	} ops[];
};

#define job_foreach_bo(_obj, _job) \
	for (unsigned int i = 0; i < (_job)->nr_ops; i++) \
		if (((_obj) = (_job)->ops[i].obj))

static inline struct msm_vm_bind_job *to_msm_vm_bind_job(struct drm_sched_job *job)
{
	return container_of(job, struct msm_vm_bind_job, base);
}

struct op_arg {
	unsigned int flags;
	struct msm_vm_bind_job *job;
	const struct msm_vm_bind_op *op;
	bool kept;
};

static void
vm_op_enqueue(struct op_arg *arg, struct msm_vm_op _op)
{
	struct msm_vm_op *op = kmalloc(sizeof(*op), GFP_KERNEL);
	*op = _op;
	list_add_tail(&op->node, &arg->job->vm_ops);

	if (op->obj)
		drm_gem_object_get(op->obj);
}

static struct drm_gpuva *
vma_from_op(struct op_arg *arg, struct drm_gpuva_op_map *op)
{
	return msm_gem_vma_new(arg->job->vm, op->gem.obj, op->gem.offset,
			       op->va.addr, op->va.addr + op->va.range);
}

int msm_gem_vm_sm_step_map(struct drm_gpuva_op *op, void *_arg)
{
	struct op_arg *arg = _arg;
	struct drm_gem_object *obj = op->map.gem.obj;
	struct drm_gpuva *vma;
	struct sg_table *sgt;
	unsigned int prot;

	if (arg->kept)
		return 0;

	vma = vma_from_op(arg, &op->map);
	if (WARN_ON(IS_ERR(vma)))
		return PTR_ERR(vma);

	vm_dbg("%p:%p:%p: %016llx %016llx", vma->vm, vma, vma->gem.obj,
	       vma->va.addr, vma->va.range);

	vma->flags = ((struct op_arg *)arg)->flags;

	if (obj) {
		sgt = to_msm_bo(obj)->sgt;
		prot = msm_gem_prot(obj);
	} else {
		sgt = NULL;
		prot = IOMMU_READ | IOMMU_WRITE;
	}

	vm_op_enqueue(arg, (struct msm_vm_op){
		.op = MSM_VM_OP_MAP,
		.map = {
			.sgt = sgt,
			.iova = vma->va.addr,
			.range = vma->va.range,
			.offset = vma->gem.offset,
			.prot = prot,
			.queue_id = arg->job->queue->id,
		},
		.obj = vma->gem.obj,
	});

	to_msm_vma(vma)->mapped = true;

	return 0;
}

int msm_gem_vm_sm_step_remap(struct drm_gpuva_op *op, void *arg)
{
	struct msm_vm_bind_job *job = ((struct op_arg *)arg)->job;
	struct drm_gpuvm *vm = job->vm;
	struct drm_gpuva *orig_vma = op->remap.unmap->va;
	struct drm_gpuva *prev_vma = NULL, *next_vma = NULL;
	struct drm_gpuvm_bo *vm_bo = orig_vma->vm_bo;
	bool mapped = to_msm_vma(orig_vma)->mapped;
	unsigned int flags;

	vm_dbg("orig_vma: %p:%p:%p: %016llx %016llx", vm, orig_vma,
	       orig_vma->gem.obj, orig_vma->va.addr, orig_vma->va.range);

	if (mapped) {
		uint64_t unmap_start, unmap_range;

		drm_gpuva_op_remap_to_unmap_range(&op->remap, &unmap_start, &unmap_range);

		vm_op_enqueue(arg, (struct msm_vm_op){
			.op = MSM_VM_OP_UNMAP,
			.unmap = {
				.iova = unmap_start,
				.range = unmap_range,
				.queue_id = job->queue->id,
			},
			.obj = orig_vma->gem.obj,
		});

		/*
		 * Part of this GEM obj is still mapped, but we're going to kill the
		 * existing VMA and replace it with one or two new ones (ie. two if
		 * the unmapped range is in the middle of the existing (unmap) VMA).
		 * So just set the state to unmapped:
		 */
		to_msm_vma(orig_vma)->mapped = false;
	}

	/*
	 * Hold a ref to the vm_bo between the msm_gem_vma_close() and the
	 * creation of the new prev/next vma's, in case the vm_bo is tracked
	 * in the VM's evict list:
	 */
	if (vm_bo)
		drm_gpuvm_bo_get(vm_bo);

	/*
	 * The prev_vma and/or next_vma are replacing the unmapped vma, and
	 * therefore should preserve it's flags:
	 */
	flags = orig_vma->flags;

	msm_gem_vma_close(orig_vma);

	if (op->remap.prev) {
		prev_vma = vma_from_op(arg, op->remap.prev);
		if (WARN_ON(IS_ERR(prev_vma)))
			return PTR_ERR(prev_vma);

		vm_dbg("prev_vma: %p:%p: %016llx %016llx", vm, prev_vma,
		       prev_vma->va.addr, prev_vma->va.range);
		to_msm_vma(prev_vma)->mapped = mapped;
		prev_vma->flags = flags;
	}

	if (op->remap.next) {
		next_vma = vma_from_op(arg, op->remap.next);
		if (WARN_ON(IS_ERR(next_vma)))
			return PTR_ERR(next_vma);

		vm_dbg("next_vma: %p:%p: %016llx %016llx", vm, next_vma,
		       next_vma->va.addr, next_vma->va.range);
		to_msm_vma(next_vma)->mapped = mapped;
		next_vma->flags = flags;
	}

	if (!mapped)
		drm_gpuvm_bo_evict(vm_bo, true);

	/* Drop the previous ref: */
	drm_gpuvm_bo_put(vm_bo);

	return 0;
}

int msm_gem_vm_sm_step_unmap(struct drm_gpuva_op *op, void *_arg)
{
	struct op_arg *arg = _arg;
	struct drm_gpuva *vma = op->unmap.va;
	struct msm_gem_vma *msm_vma = to_msm_vma(vma);

	vm_dbg("%p:%p:%p: %016llx %016llx", vma->vm, vma, vma->gem.obj,
	       vma->va.addr, vma->va.range);

	/*
	 * Detect in-place remap.  Turnip does this to change the vma flags,
	 * in particular MSM_VMA_DUMP.  In this case we want to avoid actually
	 * touching the page tables, as that would require synchronization
	 * against SUBMIT jobs running on the GPU.
	 */
	if (op->unmap.keep &&
	    (arg->op->op == MSM_VM_BIND_OP_MAP) &&
	    (vma->gem.obj == arg->op->obj) &&
	    (vma->gem.offset == arg->op->obj_offset) &&
	    (vma->va.addr == arg->op->iova) &&
	    (vma->va.range == arg->op->range)) {
		/* We are only expecting a single in-place unmap+map cb pair: */
		WARN_ON(arg->kept);

		/* Leave the existing VMA in place, but signal that to the map cb: */
		arg->kept = true;

		/* Only flags are changing, so update that in-place: */
		unsigned int orig_flags = vma->flags & (DRM_GPUVA_USERBITS - 1);

		vma->flags = orig_flags | arg->flags;

		return 0;
	}

	if (!msm_vma->mapped)
		goto out_close;

	vm_op_enqueue(arg, (struct msm_vm_op){
		.op = MSM_VM_OP_UNMAP,
		.unmap = {
			.iova = vma->va.addr,
			.range = vma->va.range,
			.queue_id = arg->job->queue->id,
		},
		.obj = vma->gem.obj,
	});

	msm_vma->mapped = false;

out_close:
	msm_gem_vma_close(vma);

	return 0;
}

static struct dma_fence *
msm_vma_job_run(struct drm_sched_job *_job)
{
	struct msm_vm_bind_job *job = to_msm_vm_bind_job(_job);
	struct msm_gem_vm *vm = to_msm_vm(job->vm);
	struct drm_gem_object *obj;
	int ret = vm->unusable ? -EINVAL : 0;

	vm_dbg("");

	mutex_lock(&vm->mmu_lock);
	vm->mmu->prealloc = &job->prealloc;

	while (!list_empty(&job->vm_ops)) {
		struct msm_vm_op *op =
			list_first_entry(&job->vm_ops, struct msm_vm_op, node);

		switch (op->op) {
		case MSM_VM_OP_MAP:
			/*
			 * On error, stop trying to map new things.. but we
			 * still want to process the unmaps (or in particular,
			 * the drm_gem_object_put()s)
			 */
			if (!ret)
				ret = vm_map_op(vm, &op->map);
			break;
		case MSM_VM_OP_UNMAP:
			vm_unmap_op(vm, &op->unmap);
			break;
		}
		drm_gem_object_put(op->obj);
		list_del(&op->node);
		kfree(op);
	}

	vm->mmu->prealloc = NULL;
	mutex_unlock(&vm->mmu_lock);

	/*
	 * We failed to perform at least _some_ of the pgtable updates, so
	 * now the VM is in an undefined state.  Game over!
	 */
	if (ret)
		msm_gem_vm_unusable(job->vm);

	job_foreach_bo(obj, job) {
		msm_gem_lock(obj);
		msm_gem_unpin_locked(obj);
		msm_gem_unlock(obj);
	}

	/* VM_BIND ops are synchronous, so no fence to wait on: */
	return NULL;
}

static void
msm_vma_job_free(struct drm_sched_job *_job)
{
	struct msm_vm_bind_job *job = to_msm_vm_bind_job(_job);
	struct msm_gem_vm *vm = to_msm_vm(job->vm);
	struct drm_gem_object *obj;

	vm->mmu->funcs->prealloc_cleanup(vm->mmu, &job->prealloc);

	atomic_sub(job->prealloc.count, &vm->prealloc_throttle.in_flight);

	drm_sched_job_cleanup(_job);

	job_foreach_bo(obj, job)
		drm_gem_object_put(obj);

	msm_submitqueue_put(job->queue);
	dma_fence_put(job->fence);

	/* In error paths, we could have unexecuted ops: */
	while (!list_empty(&job->vm_ops)) {
		struct msm_vm_op *op =
			list_first_entry(&job->vm_ops, struct msm_vm_op, node);
		list_del(&op->node);
		kfree(op);
	}

	wake_up(&vm->prealloc_throttle.wait);

	kfree(job);
}

static const struct drm_sched_backend_ops msm_vm_bind_ops = {
	.run_job = msm_vma_job_run,
	.free_job = msm_vma_job_free
};

int msm_gem_vm_sched_init(struct msm_gem_vm *vm, struct drm_device *drm)
{
	struct drm_sched_init_args args = {
		.ops = &msm_vm_bind_ops,
		.num_rqs = 1,
		.credit_limit = 1,
		.timeout = MAX_SCHEDULE_TIMEOUT,
		.name = "msm-vm-bind",
		.dev = drm->dev,
	};
	int ret;

	ret = drm_sched_init(&vm->sched, &args);
	if (ret)
		return ret;

	init_waitqueue_head(&vm->prealloc_throttle.wait);

	return 0;
}

void msm_gem_vm_sched_fini(struct msm_gem_vm *vm)
{
	/* Kill the scheduler now, so we aren't racing with it for cleanup: */
	drm_sched_stop(&vm->sched, NULL);
	drm_sched_fini(&vm->sched);
}

static struct msm_vm_bind_job *
vm_bind_job_create(struct drm_device *dev, struct drm_file *file,
		   struct msm_gpu_submitqueue *queue, uint32_t nr_ops)
{
	struct msm_vm_bind_job *job;
	uint64_t sz;
	int ret;

	sz = struct_size(job, ops, nr_ops);

	if (sz > SIZE_MAX)
		return ERR_PTR(-ENOMEM);

	job = kzalloc(sz, GFP_KERNEL | __GFP_NOWARN);
	if (!job)
		return ERR_PTR(-ENOMEM);

	ret = drm_sched_job_init(&job->base, queue->entity, 1, queue,
				 file->client_id);
	if (ret) {
		kfree(job);
		return ERR_PTR(ret);
	}

	job->vm = msm_context_vm(dev, queue->ctx);
	job->queue = queue;
	INIT_LIST_HEAD(&job->vm_ops);

	return job;
}

static bool invalid_alignment(uint64_t addr)
{
	/*
	 * Technically this is about GPU alignment, not CPU alignment.  But
	 * I've not seen any qcom SoC where the SMMU does not support the
	 * CPU's smallest page size.
	 */
	return !PAGE_ALIGNED(addr);
}

static int
lookup_op(struct msm_vm_bind_job *job, const struct drm_msm_vm_bind_op *op)
{
	struct drm_device *dev = job->vm->drm;
	int i = job->nr_ops++;
	int ret = 0;

	job->ops[i].op = op->op;
	job->ops[i].handle = op->handle;
	job->ops[i].obj_offset = op->obj_offset;
	job->ops[i].iova = op->iova;
	job->ops[i].range = op->range;
	job->ops[i].flags = op->flags;

	if (op->flags & ~MSM_VM_BIND_OP_FLAGS)
		ret = UERR(EINVAL, dev, "invalid flags: %x\n", op->flags);

	if (invalid_alignment(op->iova))
		ret = UERR(EINVAL, dev, "invalid address: %016llx\n", op->iova);

	if (invalid_alignment(op->obj_offset))
		ret = UERR(EINVAL, dev, "invalid bo_offset: %016llx\n", op->obj_offset);

	if (invalid_alignment(op->range))
		ret = UERR(EINVAL, dev, "invalid range: %016llx\n", op->range);

	if (!drm_gpuvm_range_valid(job->vm, op->iova, op->range))
		ret = UERR(EINVAL, dev, "invalid range: %016llx, %016llx\n", op->iova, op->range);

	/*
	 * MAP must specify a valid handle.  But the handle MBZ for
	 * UNMAP or MAP_NULL.
	 */
	if (op->op == MSM_VM_BIND_OP_MAP) {
		if (!op->handle)
			ret = UERR(EINVAL, dev, "invalid handle\n");
	} else if (op->handle) {
		ret = UERR(EINVAL, dev, "handle must be zero\n");
	}

	switch (op->op) {
	case MSM_VM_BIND_OP_MAP:
	case MSM_VM_BIND_OP_MAP_NULL:
	case MSM_VM_BIND_OP_UNMAP:
		break;
	default:
		ret = UERR(EINVAL, dev, "invalid op: %u\n", op->op);
		break;
	}

	return ret;
}

/*
 * ioctl parsing, parameter validation, and GEM handle lookup
 */
static int
vm_bind_job_lookup_ops(struct msm_vm_bind_job *job, struct drm_msm_vm_bind *args,
		       struct drm_file *file, int *nr_bos)
{
	struct drm_device *dev = job->vm->drm;
	int ret = 0;
	int cnt = 0;
	int i = -1;

	if (args->nr_ops == 1) {
		/* Single op case, the op is inlined: */
		ret = lookup_op(job, &args->op);
	} else {
		for (unsigned int i = 0; i < args->nr_ops; i++) {
			struct drm_msm_vm_bind_op op;
			void __user *userptr =
				u64_to_user_ptr(args->ops + (i * sizeof(op)));

			/* make sure we don't have garbage flags, in case we hit
			 * error path before flags is initialized:
			 */
			job->ops[i].flags = 0;

			if (copy_from_user(&op, userptr, sizeof(op))) {
				ret = -EFAULT;
				break;
			}

			ret = lookup_op(job, &op);
			if (ret)
				break;
		}
	}

	if (ret) {
		job->nr_ops = 0;
		goto out;
	}

	spin_lock(&file->table_lock);

	for (i = 0; i < args->nr_ops; i++) {
		struct msm_vm_bind_op *op = &job->ops[i];
		struct drm_gem_object *obj;

		if (!op->handle) {
			op->obj = NULL;
			continue;
		}

		/*
		 * normally use drm_gem_object_lookup(), but for bulk lookup
		 * all under single table_lock just hit object_idr directly:
		 */
		obj = idr_find(&file->object_idr, op->handle);
		if (!obj) {
			ret = UERR(EINVAL, dev, "invalid handle %u at index %u\n", op->handle, i);
			goto out_unlock;
		}

		drm_gem_object_get(obj);

		op->obj = obj;
		cnt++;

		if ((op->range + op->obj_offset) > obj->size) {
			ret = UERR(EINVAL, dev, "invalid range: %016llx + %016llx > %016zx\n",
				   op->range, op->obj_offset, obj->size);
			goto out_unlock;
		}
	}

	*nr_bos = cnt;

out_unlock:
	spin_unlock(&file->table_lock);

	if (ret) {
		for (; i >= 0; i--) {
			struct msm_vm_bind_op *op = &job->ops[i];

			if (!op->obj)
				continue;

			drm_gem_object_put(op->obj);
			op->obj = NULL;
		}
	}
out:
	return ret;
}

static void
prealloc_count(struct msm_vm_bind_job *job,
	       struct msm_vm_bind_op *first,
	       struct msm_vm_bind_op *last)
{
	struct msm_mmu *mmu = to_msm_vm(job->vm)->mmu;

	if (!first)
		return;

	uint64_t start_iova = first->iova;
	uint64_t end_iova = last->iova + last->range;

	mmu->funcs->prealloc_count(mmu, &job->prealloc, start_iova, end_iova - start_iova);
}

static bool
ops_are_same_pte(struct msm_vm_bind_op *first, struct msm_vm_bind_op *next)
{
	/*
	 * Last level pte covers 2MB.. so we should merge two ops, from
	 * the PoV of figuring out how much pgtable pages to pre-allocate
	 * if they land in the same 2MB range:
	 */
	uint64_t pte_mask = ~(SZ_2M - 1);

	return ((first->iova + first->range) & pte_mask) == (next->iova & pte_mask);
}

/*
 * Determine the amount of memory to prealloc for pgtables.  For sparse images,
 * in particular, userspace plays some tricks with the order of page mappings
 * to get the desired swizzle pattern, resulting in a large # of tiny MAP ops.
 * So detect when multiple MAP operations are physically contiguous, and count
 * them as a single mapping.  Otherwise the prealloc_count() will not realize
 * they can share pagetable pages and vastly overcount.
 */
static int
vm_bind_prealloc_count(struct msm_vm_bind_job *job)
{
	struct msm_vm_bind_op *first = NULL, *last = NULL;
	struct msm_gem_vm *vm = to_msm_vm(job->vm);
	int ret;

	for (int i = 0; i < job->nr_ops; i++) {
		struct msm_vm_bind_op *op = &job->ops[i];

		/* We only care about MAP/MAP_NULL: */
		if (op->op == MSM_VM_BIND_OP_UNMAP)
			continue;

		/*
		 * If op is contiguous with last in the current range, then
		 * it becomes the new last in the range and we continue
		 * looping:
		 */
		if (last && ops_are_same_pte(last, op)) {
			last = op;
			continue;
		}

		/*
		 * If op is not contiguous with the current range, flush
		 * the current range and start anew:
		 */
		prealloc_count(job, first, last);
		first = last = op;
	}

	/* Flush the remaining range: */
	prealloc_count(job, first, last);

	/*
	 * Now that we know the needed amount to pre-alloc, throttle on pending
	 * VM_BIND jobs if we already have too much pre-alloc memory in flight
	 */
	ret = wait_event_interruptible(
			vm->prealloc_throttle.wait,
			atomic_read(&vm->prealloc_throttle.in_flight) <= 1024);
	if (ret)
		return ret;

	atomic_add(job->prealloc.count, &vm->prealloc_throttle.in_flight);

	return 0;
}

/*
 * Lock VM and GEM objects
 */
static int
vm_bind_job_lock_objects(struct msm_vm_bind_job *job, struct drm_exec *exec)
{
	int ret;

	/* Lock VM and objects: */
	drm_exec_until_all_locked(exec) {
		ret = drm_exec_lock_obj(exec, drm_gpuvm_resv_obj(job->vm));
		drm_exec_retry_on_contention(exec);
		if (ret)
			return ret;

		for (unsigned int i = 0; i < job->nr_ops; i++) {
			const struct msm_vm_bind_op *op = &job->ops[i];

			switch (op->op) {
			case MSM_VM_BIND_OP_UNMAP:
				ret = drm_gpuvm_sm_unmap_exec_lock(job->vm, exec,
							      op->iova,
							      op->obj_offset);
				break;
			case MSM_VM_BIND_OP_MAP:
			case MSM_VM_BIND_OP_MAP_NULL: {
				struct drm_gpuvm_map_req map_req = {
					.map.va.addr = op->iova,
					.map.va.range = op->range,
					.map.gem.obj = op->obj,
					.map.gem.offset = op->obj_offset,
				};

				ret = drm_gpuvm_sm_map_exec_lock(job->vm, exec, 1, &map_req);
				break;
			}
			default:
				/*
				 * lookup_op() should have already thrown an error for
				 * invalid ops
				 */
				WARN_ON("unreachable");
			}

			drm_exec_retry_on_contention(exec);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/*
 * Pin GEM objects, ensuring that we have backing pages.  Pinning will move
 * the object to the pinned LRU so that the shrinker knows to first consider
 * other objects for evicting.
 */
static int
vm_bind_job_pin_objects(struct msm_vm_bind_job *job)
{
	struct drm_gem_object *obj;

	/*
	 * First loop, before holding the LRU lock, avoids holding the
	 * LRU lock while calling msm_gem_pin_vma_locked (which could
	 * trigger get_pages())
	 */
	job_foreach_bo(obj, job) {
		struct page **pages;

		pages = msm_gem_get_pages_locked(obj, MSM_MADV_WILLNEED);
		if (IS_ERR(pages))
			return PTR_ERR(pages);
	}

	struct msm_drm_private *priv = job->vm->drm->dev_private;

	/*
	 * A second loop while holding the LRU lock (a) avoids acquiring/dropping
	 * the LRU lock for each individual bo, while (b) avoiding holding the
	 * LRU lock while calling msm_gem_pin_vma_locked() (which could trigger
	 * get_pages() which could trigger reclaim.. and if we held the LRU lock
	 * could trigger deadlock with the shrinker).
	 */
	mutex_lock(&priv->lru.lock);
	job_foreach_bo(obj, job)
		msm_gem_pin_obj_locked(obj);
	mutex_unlock(&priv->lru.lock);

	job->bos_pinned = true;

	return 0;
}

/*
 * Unpin GEM objects.  Normally this is done after the bind job is run.
 */
static void
vm_bind_job_unpin_objects(struct msm_vm_bind_job *job)
{
	struct drm_gem_object *obj;

	if (!job->bos_pinned)
		return;

	job_foreach_bo(obj, job)
		msm_gem_unpin_locked(obj);

	job->bos_pinned = false;
}

/*
 * Pre-allocate pgtable memory, and translate the VM bind requests into a
 * sequence of pgtable updates to be applied asynchronously.
 */
static int
vm_bind_job_prepare(struct msm_vm_bind_job *job)
{
	struct msm_gem_vm *vm = to_msm_vm(job->vm);
	struct msm_mmu *mmu = vm->mmu;
	int ret;

	ret = mmu->funcs->prealloc_allocate(mmu, &job->prealloc);
	if (ret)
		return ret;

	for (unsigned int i = 0; i < job->nr_ops; i++) {
		const struct msm_vm_bind_op *op = &job->ops[i];
		struct op_arg arg = {
			.job = job,
			.op = op,
		};

		switch (op->op) {
		case MSM_VM_BIND_OP_UNMAP:
			ret = drm_gpuvm_sm_unmap(job->vm, &arg, op->iova,
						 op->range);
			break;
		case MSM_VM_BIND_OP_MAP:
			if (op->flags & MSM_VM_BIND_OP_DUMP)
				arg.flags |= MSM_VMA_DUMP;
			fallthrough;
		case MSM_VM_BIND_OP_MAP_NULL: {
			struct drm_gpuvm_map_req map_req = {
				.map.va.addr = op->iova,
				.map.va.range = op->range,
				.map.gem.obj = op->obj,
				.map.gem.offset = op->obj_offset,
			};

			ret = drm_gpuvm_sm_map(job->vm, &arg, &map_req);
			break;
		}
		default:
			/*
			 * lookup_op() should have already thrown an error for
			 * invalid ops
			 */
			BUG_ON("unreachable");
		}

		if (ret) {
			/*
			 * If we've already started modifying the vm, we can't
			 * adequetly describe to userspace the intermediate
			 * state the vm is in.  So throw up our hands!
			 */
			if (i > 0)
				msm_gem_vm_unusable(job->vm);
			return ret;
		}
	}

	return 0;
}

/*
 * Attach fences to the GEM objects being bound.  This will signify to
 * the shrinker that they are busy even after dropping the locks (ie.
 * drm_exec_fini())
 */
static void
vm_bind_job_attach_fences(struct msm_vm_bind_job *job)
{
	for (unsigned int i = 0; i < job->nr_ops; i++) {
		struct drm_gem_object *obj = job->ops[i].obj;

		if (!obj)
			continue;

		dma_resv_add_fence(obj->resv, job->fence,
				   DMA_RESV_USAGE_KERNEL);
	}
}

int
msm_ioctl_vm_bind(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_msm_vm_bind *args = data;
	struct msm_context *ctx = file->driver_priv;
	struct msm_vm_bind_job *job = NULL;
	struct msm_gpu *gpu = priv->gpu;
	struct msm_gpu_submitqueue *queue;
	struct msm_syncobj_post_dep *post_deps = NULL;
	struct drm_syncobj **syncobjs_to_reset = NULL;
	struct sync_file *sync_file = NULL;
	struct dma_fence *fence;
	int out_fence_fd = -1;
	int ret, nr_bos = 0;
	unsigned int i;

	if (!gpu)
		return -ENXIO;

	/*
	 * Maybe we could allow just UNMAP ops?  OTOH userspace should just
	 * immediately close the device file and all will be torn down.
	 */
	if (to_msm_vm(ctx->vm)->unusable)
		return UERR(EPIPE, dev, "context is unusable");

	/*
	 * Technically, you cannot create a VM_BIND submitqueue in the first
	 * place, if you haven't opted in to VM_BIND context.  But it is
	 * cleaner / less confusing, to check this case directly.
	 */
	if (!msm_context_is_vmbind(ctx))
		return UERR(EINVAL, dev, "context does not support vmbind");

	if (args->flags & ~MSM_VM_BIND_FLAGS)
		return UERR(EINVAL, dev, "invalid flags");

	queue = msm_submitqueue_get(ctx, args->queue_id);
	if (!queue)
		return -ENOENT;

	if (!(queue->flags & MSM_SUBMITQUEUE_VM_BIND)) {
		ret = UERR(EINVAL, dev, "Invalid queue type");
		goto out_post_unlock;
	}

	if (args->flags & MSM_VM_BIND_FENCE_FD_OUT) {
		out_fence_fd = get_unused_fd_flags(O_CLOEXEC);
		if (out_fence_fd < 0) {
			ret = out_fence_fd;
			goto out_post_unlock;
		}
	}

	job = vm_bind_job_create(dev, file, queue, args->nr_ops);
	if (IS_ERR(job)) {
		ret = PTR_ERR(job);
		goto out_post_unlock;
	}

	ret = mutex_lock_interruptible(&queue->lock);
	if (ret)
		goto out_post_unlock;

	if (args->flags & MSM_VM_BIND_FENCE_FD_IN) {
		struct dma_fence *in_fence;

		in_fence = sync_file_get_fence(args->fence_fd);

		if (!in_fence) {
			ret = UERR(EINVAL, dev, "invalid in-fence");
			goto out_unlock;
		}

		ret = drm_sched_job_add_dependency(&job->base, in_fence);
		if (ret)
			goto out_unlock;
	}

	if (args->in_syncobjs > 0) {
		syncobjs_to_reset = msm_syncobj_parse_deps(dev, &job->base,
							   file, args->in_syncobjs,
							   args->nr_in_syncobjs,
							   args->syncobj_stride);
		if (IS_ERR(syncobjs_to_reset)) {
			ret = PTR_ERR(syncobjs_to_reset);
			goto out_unlock;
		}
	}

	if (args->out_syncobjs > 0) {
		post_deps = msm_syncobj_parse_post_deps(dev, file,
							args->out_syncobjs,
							args->nr_out_syncobjs,
							args->syncobj_stride);
		if (IS_ERR(post_deps)) {
			ret = PTR_ERR(post_deps);
			goto out_unlock;
		}
	}

	ret = vm_bind_job_lookup_ops(job, args, file, &nr_bos);
	if (ret)
		goto out_unlock;

	ret = vm_bind_prealloc_count(job);
	if (ret)
		goto out_unlock;

	struct drm_exec exec;
	unsigned int flags = DRM_EXEC_IGNORE_DUPLICATES | DRM_EXEC_INTERRUPTIBLE_WAIT;

	drm_exec_init(&exec, flags, nr_bos + 1);

	ret = vm_bind_job_lock_objects(job, &exec);
	if (ret)
		goto out;

	ret = vm_bind_job_pin_objects(job);
	if (ret)
		goto out;

	ret = vm_bind_job_prepare(job);
	if (ret)
		goto out;

	drm_sched_job_arm(&job->base);

	job->fence = dma_fence_get(&job->base.s_fence->finished);

	if (args->flags & MSM_VM_BIND_FENCE_FD_OUT) {
		sync_file = sync_file_create(job->fence);
		if (!sync_file)
			ret = -ENOMEM;
	}

	if (ret)
		goto out;

	vm_bind_job_attach_fences(job);

	/*
	 * The job can be free'd (and fence unref'd) at any point after
	 * drm_sched_entity_push_job(), so we need to hold our own ref
	 */
	fence = dma_fence_get(job->fence);

	drm_sched_entity_push_job(&job->base);

	msm_syncobj_reset(syncobjs_to_reset, args->nr_in_syncobjs);
	msm_syncobj_process_post_deps(post_deps, args->nr_out_syncobjs, fence);

	dma_fence_put(fence);

out:
	if (ret)
		vm_bind_job_unpin_objects(job);

	drm_exec_fini(&exec);
out_unlock:
	mutex_unlock(&queue->lock);
out_post_unlock:
	if (ret) {
		if (out_fence_fd >= 0)
			put_unused_fd(out_fence_fd);
		if (sync_file)
			fput(sync_file->file);
	} else if (sync_file) {
		fd_install(out_fence_fd, sync_file->file);
		args->fence_fd = out_fence_fd;
	}

	if (!IS_ERR_OR_NULL(job)) {
		if (ret)
			msm_vma_job_free(&job->base);
	} else {
		/*
		 * If the submit hasn't yet taken ownership of the queue
		 * then we need to drop the reference ourself:
		 */
		msm_submitqueue_put(queue);
	}

	if (!IS_ERR_OR_NULL(post_deps)) {
		for (i = 0; i < args->nr_out_syncobjs; ++i) {
			kfree(post_deps[i].chain);
			drm_syncobj_put(post_deps[i].syncobj);
		}
		kfree(post_deps);
	}

	if (!IS_ERR_OR_NULL(syncobjs_to_reset)) {
		for (i = 0; i < args->nr_in_syncobjs; ++i) {
			if (syncobjs_to_reset[i])
				drm_syncobj_put(syncobjs_to_reset[i]);
		}
		kfree(syncobjs_to_reset);
	}

	return ret;
}
