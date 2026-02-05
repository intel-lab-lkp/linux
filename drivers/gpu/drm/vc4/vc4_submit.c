// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright © 2014 Broadcom
 * Copyright © 2026 Raspberry Pi
 */

#include <drm/drm_exec.h>
#include <drm/drm_print.h>
#include <drm/drm_syncobj.h>

#include "vc4_drv.h"
#include "vc4_regs.h"
#include "vc4_trace.h"

/* Takes the reservation lock on all the BOs being referenced, so that
 * at queue submit time we can update the reservations.
 *
 * We don't lock the RCL the tile alloc/state BOs, or overflow memory
 * (all of which are on render->unref_list). They're entirely private
 * to vc4, so we don't attach dma-buf fences to them.
 */
static int
vc4_lock_bo_reservations(struct vc4_render_job *job, struct drm_exec *exec)
{
	int ret;

	/* Reserve space for our shared (read-only) fence references,
	 * before we commit the CL to the hardware.
	 */
	drm_exec_init(exec, DRM_EXEC_INTERRUPTIBLE_WAIT, job->bo_count);
	drm_exec_until_all_locked(exec) {
		ret = drm_exec_prepare_array(exec, job->bo, job->bo_count, 1);
	}

	if (ret)
		goto fail;

	for (int i = 0; i < job->bo_count; i++) {
		ret = drm_sched_job_add_implicit_dependencies(&job->base.base,
							      job->bo[i], true);
		if (ret)
			goto fail;
	}

	return 0;
fail:
	drm_exec_fini(exec);
	return ret;
}

/**
 * vc4_lookup_bos() - Sets up job->bo[] with the GEM objects
 * referenced by the job.
 * @dev: DRM device
 * @file_priv: DRM file for this fd
 * @job: VC4 render job being set up
 * @bo_handles: GEM handles
 * @bo_count: Number of GEM handles passed in
 *
 * The command validator needs to reference BOs by their index within
 * the submitted job's BO list. This does the validation of the job's
 * BO list and reference counting for the lifetime of the job.
 */
static int
vc4_lookup_bos(struct drm_device *dev, struct drm_file *file_priv,
	       struct vc4_render_job *job, u64 bo_handles, u32 bo_count)
{
	int ret = 0;
	int i;

	job->bo_count = bo_count;

	if (!job->bo_count) {
		drm_warn(dev, "Rendering requires BOs to validate\n");
		return -EINVAL;
	}

	ret = drm_gem_objects_lookup(file_priv, u64_to_user_ptr(bo_handles),
				     job->bo_count, &job->bo);
	if (ret)
		return ret;

	for (i = 0; i < job->bo_count; i++) {
		ret = vc4_bo_inc_usecnt(to_vc4_bo(job->bo[i]));
		if (ret)
			goto fail_dec_usecnt;
	}

	return 0;

fail_dec_usecnt:
	/* Decrease usecnt on acquired objects */
	for (i-- ; i >= 0; i--)
		vc4_bo_dec_usecnt(to_vc4_bo(job->bo[i]));
	return ret;
}

static int
vc4_get_bcl(struct drm_device *dev, struct vc4_exec_info *exec)
{
	struct drm_vc4_submit_cl *args = exec->args;
	struct vc4_render_job *render_job = exec->render;
	struct vc4_bin_job *bin_job = exec->bin;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_gem_dma_object *exec_bo;
	void *temp = NULL;
	void *bin;
	int ret = 0;
	uint32_t bin_offset = 0;
	uint32_t shader_rec_offset = roundup(bin_offset + args->bin_cl_size, 16);
	uint32_t uniforms_offset = shader_rec_offset + args->shader_rec_size;
	uint32_t exec_size = uniforms_offset + args->uniforms_size;
	uint32_t temp_size = exec_size + (sizeof(struct vc4_shader_state) *
					  args->shader_rec_count);
	struct vc4_bo *bo;

	if (shader_rec_offset < args->bin_cl_size ||
	    uniforms_offset < shader_rec_offset ||
	    exec_size < uniforms_offset ||
	    args->shader_rec_count >= (UINT_MAX /
					  sizeof(struct vc4_shader_state)) ||
	    temp_size < exec_size) {
		drm_dbg(dev, "overflow in exec arguments\n");
		ret = -EINVAL;
		goto fail;
	}

	/* Allocate space where we'll store the copied in user command lists
	 * and shader records.
	 *
	 * We don't just copy directly into the BOs because we need to
	 * read the contents back for validation, and I think the
	 * bo->vaddr is uncached access.
	 */
	temp = kvmalloc_array(temp_size, 1, GFP_KERNEL);
	if (!temp) {
		drm_err(dev, "Failed to allocate storage for copying in bin/render CLs.");
		ret = -ENOMEM;
		goto fail;
	}
	bin = temp + bin_offset;
	exec->shader_rec_u = temp + shader_rec_offset;
	exec->uniforms_u = temp + uniforms_offset;
	exec->shader_state = temp + exec_size;
	exec->shader_state_size = args->shader_rec_count;

	if (copy_from_user(bin, u64_to_user_ptr(args->bin_cl),
			   args->bin_cl_size)) {
		ret = -EFAULT;
		goto fail;
	}

	if (copy_from_user(exec->shader_rec_u, u64_to_user_ptr(args->shader_rec),
			   args->shader_rec_size)) {
		ret = -EFAULT;
		goto fail;
	}

	if (copy_from_user(exec->uniforms_u, u64_to_user_ptr(args->uniforms),
			   args->uniforms_size)) {
		ret = -EFAULT;
		goto fail;
	}

	bo = vc4_bo_create(dev, exec_size, true, VC4_BO_TYPE_BCL);
	if (IS_ERR(bo)) {
		drm_err(dev, "Couldn't allocate BO for binning\n");
		ret = PTR_ERR(bo);
		goto fail;
	}
	exec_bo = &bo->base;

	list_add_tail(&bo->unref_head, &render_job->unref_list);

	bin_job->ct0ca = exec_bo->dma_addr + bin_offset;

	exec->bin_u = bin;

	exec->shader_rec_v = exec_bo->vaddr + shader_rec_offset;
	exec->shader_rec_p = exec_bo->dma_addr + shader_rec_offset;
	exec->shader_rec_size = args->shader_rec_size;

	exec->uniforms_v = exec_bo->vaddr + uniforms_offset;
	exec->uniforms_p = exec_bo->dma_addr + uniforms_offset;
	exec->uniforms_size = args->uniforms_size;

	ret = vc4_validate_bin_cl(dev, exec_bo->vaddr + bin_offset, bin, exec);
	if (ret)
		goto fail;

	ret = vc4_validate_shader_recs(dev, exec);
	if (ret)
		goto fail;

	if (exec->found_tile_binning_mode_config_packet) {
		ret = vc4_v3d_bin_bo_get(vc4, &bin_job->bin_bo_used);
		if (ret)
			goto fail;
	}

fail:
	kvfree(temp);
	return ret;
}

static void
vc4_job_free(struct kref *ref)
{
	struct vc4_job *job = container_of(ref, struct vc4_job, refcount);

	dma_fence_put(job->irq_fence);
	dma_fence_put(job->done_fence);

	if (job->perfmon)
		vc4_perfmon_put(job->perfmon);

	vc4_v3d_pm_put(job->vc4);
	kfree(job);
}

static void
vc4_bin_job_free(struct kref *ref)
{
	struct vc4_bin_job *job = container_of(ref, struct vc4_bin_job,
					       base.refcount);
	struct vc4_dev *vc4 = job->base.vc4;

	/* Release the reference on the binner BO if needed. */
	if (job->bin_bo_used)
		vc4_v3d_bin_bo_put(vc4);

	vc4_job_free(ref);
}

static void
vc4_render_job_free(struct kref *ref)
{
	struct vc4_render_job *job = container_of(ref, struct vc4_render_job,
						  base.refcount);
	struct vc4_dev *vc4 = job->base.vc4;
	struct vc4_bo *bo, *tmp;
	unsigned long irqflags;
	int i;

	if (job->bo) {
		for (i = 0; i < job->bo_count; i++) {
			struct vc4_bo *bo = to_vc4_bo(job->bo[i]);

			vc4_bo_dec_usecnt(bo);
			drm_gem_object_put(job->bo[i]);
		}
		kvfree(job->bo);
	}

	list_for_each_entry_safe(bo, tmp, &job->unref_list, unref_head) {
		list_del(&bo->unref_head);
		drm_gem_object_put(&bo->base.base);
	}

	/* Free up the allocation of any bin slots we used. */
	spin_lock_irqsave(&vc4->job_lock, irqflags);
	vc4->bin_alloc_used &= ~job->bin_slots;
	spin_unlock_irqrestore(&vc4->job_lock, irqflags);

	vc4_job_free(ref);
}

static void
vc4_job_put(struct vc4_job *job)
{
	if (!job)
		return;

	kref_put(&job->refcount, job->free);
}

void vc4_job_cleanup(struct vc4_job *job)
{
	if (!job)
		return;

	drm_sched_job_cleanup(&job->base);
	vc4_job_put(job);
}

static int
vc4_job_init(struct vc4_dev *vc4, struct drm_file *file_priv,
	     void **container, size_t size, void (*free)(struct kref *ref),
	     u32 in_sync, enum vc4_queue queue)
{
	struct vc4_file *vc4_priv = file_priv->driver_priv;
	struct vc4_job *job;
	int ret;

	*container = kcalloc(1, size, GFP_KERNEL);
	if (!*container) {
		drm_err(&vc4->base, "Cannot allocate memory for VC4 job.");
		return -ENOMEM;
	}

	job = *container;
	job->vc4 = vc4;
	job->free = free;

	ret = drm_sched_job_init(&job->base, &vc4_priv->sched_entity[queue],
				 1, vc4_priv, file_priv->client_id);
	if (ret)
		goto fail;

	ret = drm_sched_job_add_syncobj_dependency(&job->base, file_priv,
						   in_sync, 0);
	if (ret && ret != -ENOENT)
		goto fail_deps;

	ret = vc4_v3d_pm_get(vc4);
	if (ret)
		goto fail_deps;

	kref_init(&job->refcount);

	return 0;

fail_deps:
	drm_sched_job_cleanup(&job->base);
fail:
	kfree(*container);
	*container = NULL;

	return ret;
}

static void
vc4_push_job(struct vc4_job *job)
{
	drm_sched_job_arm(&job->base);

	job->done_fence = dma_fence_get(&job->base.s_fence->finished);

	/* put by scheduler job completion */
	kref_get(&job->refcount);

	drm_sched_entity_push_job(&job->base);
}

static void
vc4_attach_fences_and_unlock_reservation(struct drm_file *file_priv,
					 struct vc4_render_job *job,
					 struct drm_exec *exec,
					 u32 out_sync,
					 struct dma_fence *done_fence)
{
	struct drm_syncobj *sync_out;
	struct vc4_bo *bo;
	int i;

	for (i = 0; i < job->bo_count; i++) {
		bo = to_vc4_bo(job->bo[i]);
		dma_resv_add_fence(bo->base.base.resv, job->base.done_fence,
				   DMA_RESV_USAGE_READ);
	}

	for (i = 0; i < job->rcl_write_bo_count; i++) {
		bo = to_vc4_bo(&job->rcl_write_bo[i]->base);
		dma_resv_add_fence(bo->base.base.resv, job->base.done_fence,
				   DMA_RESV_USAGE_WRITE);
	}

	drm_exec_fini(exec);

	/* Update the return sync object for the job */
	sync_out = drm_syncobj_find(file_priv, out_sync);
	if (sync_out) {
		drm_syncobj_replace_fence(sync_out, done_fence);
		drm_syncobj_put(sync_out);
	}
}

/**
 * vc4_submit_cl_ioctl() - Submits a job (frame) to VC4.
 * @dev: DRM device
 * @data: ioctl argument
 * @file_priv: DRM file for this fd
 *
 * This is the main entrypoint for userspace to submit a 3D frame to
 * the GPU. Userspace provides the binner command list (if
 * applicable), and the kernel sets up the render command list to draw
 * to the framebuffer described in the ioctl, using the command lists
 * that the 3D engine's binner will produce.
 */
int
vc4_submit_cl_ioctl(struct drm_device *dev, void *data,
		    struct drm_file *file_priv)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_file *vc4_priv = file_priv->driver_priv;
	struct drm_vc4_submit_cl *args = data;
	struct vc4_exec_info exec = {
		.dev = vc4,
		.args = args,
	};
	struct vc4_bin_job *bin = NULL;
	struct vc4_render_job *render = NULL;
	struct drm_exec exec_ctx;
	int ret = 0;

	trace_vc4_submit_cl_ioctl(dev, args->bin_cl_size, args->shader_rec_size,
				  args->bo_handle_count);

	if (WARN_ON_ONCE(vc4->gen > VC4_GEN_4))
		return -ENODEV;

	if (!vc4->v3d) {
		drm_dbg(dev, "VC4_SUBMIT_CL with no VC4 V3D probed\n");
		return -ENODEV;
	}

	if ((args->flags & ~(VC4_SUBMIT_CL_USE_CLEAR_COLOR |
			     VC4_SUBMIT_CL_FIXED_RCL_ORDER |
			     VC4_SUBMIT_CL_RCL_ORDER_INCREASING_X |
			     VC4_SUBMIT_CL_RCL_ORDER_INCREASING_Y)) != 0) {
		drm_dbg(dev, "Unknown flags: 0x%02x\n", args->flags);
		return -EINVAL;
	}

	if (args->pad2 != 0) {
		drm_dbg(dev, "Invalid pad: 0x%08x\n", args->pad2);
		return -EINVAL;
	}

	ret = vc4_job_init(vc4, file_priv, (void *)&render, sizeof(*render),
			   vc4_render_job_free, args->in_sync, VC4_RENDER);
	if (ret)
		return ret;

	exec.render = render;
	INIT_LIST_HEAD(&render->unref_list);

	ret = vc4_lookup_bos(dev, file_priv, render, args->bo_handles,
			     args->bo_handle_count);
	if (ret)
		goto fail;

	if (args->bin_cl_size != 0) {
		ret = vc4_job_init(vc4, file_priv, (void *)&bin, sizeof(*bin),
				   vc4_bin_job_free, args->in_sync, VC4_BIN);
		if (ret)
			goto fail;

		exec.bin = bin;
		bin->render = render;

		ret = vc4_get_bcl(dev, &exec);
		if (ret)
			goto fail;
	}

	ret = vc4_get_rcl(dev, &exec);
	if (ret)
		goto fail;

	ret = vc4_lock_bo_reservations(render, &exec_ctx);
	if (ret)
		goto fail;

	if (args->perfmonid) {
		render->base.perfmon = vc4_perfmon_find(vc4_priv, args->perfmonid);
		if (!render->base.perfmon) {
			ret = -ENOENT;
			goto fail_perfmon;
		}

		if (bin) {
			bin->base.perfmon = render->base.perfmon;
			vc4_perfmon_get(bin->base.perfmon);
		}
	}

	mutex_lock(&vc4->sched_lock);
	if (bin) {
		vc4_push_job(&bin->base);

		ret = drm_sched_job_add_dependency(&render->base.base,
						   dma_fence_get(bin->base.done_fence));
		if (ret)
			goto fail_unreserve;
	}

	vc4_push_job(&render->base);
	mutex_unlock(&vc4->sched_lock);

	vc4_attach_fences_and_unlock_reservation(file_priv, render,
						 &exec_ctx, args->out_sync,
						 render->base.done_fence);

	vc4_job_put((void *)bin);
	vc4_job_put((void *)render);

	return 0;

fail_unreserve:
	mutex_unlock(&vc4->sched_lock);
fail_perfmon:
	drm_exec_fini(&exec_ctx);
fail:
	vc4_job_cleanup((void *)bin);
	vc4_job_cleanup((void *)render);

	return ret;
}
