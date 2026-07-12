/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <drm/drm_print.h>

#include "vc4_drv.h"
#include "vc4_regs.h"
#include "vc4_trace.h"

struct vc4_hang_state {
	struct drm_vc4_get_hang_state user_state;

	u32 bo_count;
	struct drm_gem_object **bo;
};

static void
vc4_free_hang_state(struct drm_device *dev, struct vc4_hang_state *state)
{
	unsigned int i;

	for (i = 0; i < state->user_state.bo_count; i++)
		drm_gem_object_put(state->bo[i]);

	kfree(state->bo);
	kfree(state);
}

int
vc4_get_hang_state_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv)
{
	struct drm_vc4_get_hang_state *get_state = data;
	struct drm_vc4_get_hang_state_bo *bo_state;
	struct vc4_hang_state *kernel_state;
	struct drm_vc4_get_hang_state *state;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	unsigned long irqflags;
	u32 i;
	int ret = 0;

	if (WARN_ON_ONCE(vc4->gen > VC4_GEN_4))
		return -ENODEV;

	if (!vc4->v3d) {
		DRM_DEBUG("VC4_GET_HANG_STATE with no VC4 V3D probed\n");
		return -ENODEV;
	}

	spin_lock_irqsave(&vc4->job_lock, irqflags);
	kernel_state = vc4->hang_state;
	if (!kernel_state) {
		spin_unlock_irqrestore(&vc4->job_lock, irqflags);
		return -ENOENT;
	}
	state = &kernel_state->user_state;

	/* If the user's array isn't big enough, just return the
	 * required array size.
	 */
	if (get_state->bo_count < state->bo_count) {
		get_state->bo_count = state->bo_count;
		spin_unlock_irqrestore(&vc4->job_lock, irqflags);
		return 0;
	}

	vc4->hang_state = NULL;
	spin_unlock_irqrestore(&vc4->job_lock, irqflags);

	/* Save the user's BO pointer, so we don't stomp it with the memcpy. */
	state->bo = get_state->bo;
	memcpy(get_state, state, sizeof(*state));

	bo_state = kzalloc_objs(*bo_state, state->bo_count);
	if (!bo_state) {
		ret = -ENOMEM;
		goto err_free;
	}

	for (i = 0; i < state->bo_count; i++) {
		struct vc4_bo *vc4_bo = to_vc4_bo(kernel_state->bo[i]);
		u32 handle;

		ret = drm_gem_handle_create(file_priv, kernel_state->bo[i],
					    &handle);

		if (ret) {
			state->bo_count = i;
			goto err_delete_handle;
		}
		bo_state[i].handle = handle;
		bo_state[i].paddr = vc4_bo->base.dma_addr;
		bo_state[i].size = vc4_bo->base.base.size;
	}

	if (copy_to_user(u64_to_user_ptr(get_state->bo),
			 bo_state,
			 state->bo_count * sizeof(*bo_state)))
		ret = -EFAULT;

err_delete_handle:
	if (ret) {
		for (i = 0; i < state->bo_count; i++)
			drm_gem_handle_delete(file_priv, bo_state[i].handle);
	}

err_free:
	vc4_free_hang_state(dev, kernel_state);
	kfree(bo_state);

	return ret;
}

void
vc4_save_hang_state(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_vc4_get_hang_state *state;
	struct vc4_hang_state *kernel_state;
	struct vc4_bin_job *bin_job;
	struct vc4_render_job *render_job;
	struct vc4_bo *bo;
	unsigned long irqflags;
	unsigned int i, k, unref_list_count;

	kernel_state = kzalloc_objs(*kernel_state, 1);
	if (!kernel_state)
		return;

	state = &kernel_state->user_state;

	spin_lock_irqsave(&vc4->job_lock, irqflags);
	bin_job = vc4->bin_job;
	render_job = vc4->render_job;
	if (!bin_job && !render_job)
		goto err_free_state;

	/* Get the BOs from the render job into hang state. */
	state->bo_count = 0;
	if (render_job) {
		unref_list_count = list_count_nodes(&render_job->unref_list);
		state->bo_count += render_job->bo_count + unref_list_count;
	}

	kernel_state->bo = kzalloc_objs(*kernel_state->bo, state->bo_count,
					GFP_ATOMIC);

	if (!kernel_state->bo)
		goto err_free_state;

	k = 0;
	if (render_job) {
		for (i = 0; i < render_job->bo_count; i++) {
			bo = to_vc4_bo(render_job->bo[i]);

			/* Retain BOs just in case they were marked purgeable.
			 * This prevents the BO from being purged before
			 * someone had a chance to dump the hang state.
			 */
			WARN_ON(!refcount_read(&bo->usecnt));
			refcount_inc(&bo->usecnt);
			drm_gem_object_get(render_job->bo[i]);
			kernel_state->bo[k++] = render_job->bo[i];
		}

		list_for_each_entry(bo, &render_job->unref_list, unref_head) {
			/* No need to retain BOs coming from the ->unref_list
			 * because they are naturally unpurgeable.
			 */
			drm_gem_object_get(&bo->base.base);
			kernel_state->bo[k++] = &bo->base.base;
		}
	}

	WARN_ON_ONCE(k != state->bo_count);

	if (bin_job)
		state->start_bin = bin_job->ct0ca;
	if (render_job)
		state->start_render = render_job->ct1ca;

	spin_unlock_irqrestore(&vc4->job_lock, irqflags);

	state->ct0ca = V3D_READ(V3D_CTNCA(0));
	state->ct0ea = V3D_READ(V3D_CTNEA(0));

	state->ct1ca = V3D_READ(V3D_CTNCA(1));
	state->ct1ea = V3D_READ(V3D_CTNEA(1));

	state->ct0cs = V3D_READ(V3D_CTNCS(0));
	state->ct1cs = V3D_READ(V3D_CTNCS(1));

	state->ct0ra0 = V3D_READ(V3D_CT00RA0);
	state->ct1ra0 = V3D_READ(V3D_CT01RA0);

	state->bpca = V3D_READ(V3D_BPCA);
	state->bpcs = V3D_READ(V3D_BPCS);
	state->bpoa = V3D_READ(V3D_BPOA);
	state->bpos = V3D_READ(V3D_BPOS);

	state->vpmbase = V3D_READ(V3D_VPMBASE);

	state->dbge = V3D_READ(V3D_DBGE);
	state->fdbgo = V3D_READ(V3D_FDBGO);
	state->fdbgb = V3D_READ(V3D_FDBGB);
	state->fdbgr = V3D_READ(V3D_FDBGR);
	state->fdbgs = V3D_READ(V3D_FDBGS);
	state->errstat = V3D_READ(V3D_ERRSTAT);

	/* We need to turn purgeable BOs into unpurgeable ones so that
	 * userspace has a chance to dump the hang state before the kernel
	 * decides to purge those BOs.
	 * Note that BO consistency at dump time cannot be guaranteed. For
	 * example, if the owner of these BOs decides to re-use them or mark
	 * them purgeable again there's nothing we can do to prevent it.
	 */
	for (i = 0; i < kernel_state->user_state.bo_count; i++) {
		struct vc4_bo *bo = to_vc4_bo(kernel_state->bo[i]);

		if (bo->madv == __VC4_MADV_NOTSUPP)
			continue;

		mutex_lock(&bo->madv_lock);
		if (!WARN_ON(bo->madv == __VC4_MADV_PURGED))
			bo->madv = VC4_MADV_WILLNEED;
		refcount_dec(&bo->usecnt);
		mutex_unlock(&bo->madv_lock);
	}

	spin_lock_irqsave(&vc4->job_lock, irqflags);
	if (vc4->hang_state) {
		spin_unlock_irqrestore(&vc4->job_lock, irqflags);
		vc4_free_hang_state(dev, kernel_state);
	} else {
		vc4->hang_state = kernel_state;
		spin_unlock_irqrestore(&vc4->job_lock, irqflags);
	}

	return;

err_free_state:
	spin_unlock_irqrestore(&vc4->job_lock, irqflags);
	kfree(kernel_state);
}

static void vc4_gem_destroy(struct drm_device *dev, void *unused);
int vc4_gem_init(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int ret;

	if (WARN_ON_ONCE(vc4->gen > VC4_GEN_4))
		return -ENODEV;

	spin_lock_init(&vc4->job_lock);
	spin_lock_init(&vc4->perfmon_state.lock);
	ret = drmm_mutex_init(dev, &vc4->reset_lock);
	if (ret)
		return ret;
	ret = drmm_mutex_init(dev, &vc4->sched_lock);
	if (ret)
		return ret;

	ret = drmm_mutex_init(dev, &vc4->power_lock);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&vc4->purgeable.list);

	ret = drmm_mutex_init(dev, &vc4->purgeable.lock);
	if (ret)
		return ret;

	ret = vc4_sched_init(vc4);
	if (ret)
		return ret;

	return drmm_add_action_or_reset(dev, vc4_gem_destroy, NULL);
}

static void vc4_gem_destroy(struct drm_device *dev, void *unused)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	/* Waiting for exec to finish would need to be done before
	 * unregistering V3D.
	 */
	WARN_ON(vc4->bin_job);
	WARN_ON(vc4->render_job);

	/* V3D should already have disabled its interrupt and cleared
	 * the overflow allocation registers.  Now free the object.
	 */
	if (vc4->bin_bo) {
		drm_gem_object_put(&vc4->bin_bo->base.base);
		vc4->bin_bo = NULL;
	}

	if (vc4->hang_state)
		vc4_free_hang_state(dev, vc4->hang_state);

	vc4_sched_fini(vc4);
}

int vc4_gem_madvise_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_vc4_gem_madvise *args = data;
	struct drm_gem_object *gem_obj;
	struct vc4_bo *bo;
	int ret;

	if (WARN_ON_ONCE(vc4->gen > VC4_GEN_4))
		return -ENODEV;

	switch (args->madv) {
	case VC4_MADV_DONTNEED:
	case VC4_MADV_WILLNEED:
		break;
	default:
		return -EINVAL;
	}

	if (args->pad != 0)
		return -EINVAL;

	gem_obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!gem_obj) {
		DRM_DEBUG("Failed to look up GEM BO %d\n", args->handle);
		return -ENOENT;
	}

	bo = to_vc4_bo(gem_obj);

	/* Only BOs exposed to userspace can be purged. */
	if (bo->madv == __VC4_MADV_NOTSUPP) {
		DRM_DEBUG("madvise not supported on this BO\n");
		ret = -EINVAL;
		goto out_put_gem;
	}

	/* Not sure it's safe to purge imported BOs. Let's just assume it's
	 * not until proven otherwise.
	 */
	if (drm_gem_is_imported(gem_obj)) {
		DRM_DEBUG("madvise not supported on imported BOs\n");
		ret = -EINVAL;
		goto out_put_gem;
	}

	mutex_lock(&bo->madv_lock);

	if (args->madv == VC4_MADV_DONTNEED && bo->madv == VC4_MADV_WILLNEED &&
	    !refcount_read(&bo->usecnt)) {
		/* If the BO is about to be marked as purgeable, is not used
		 * and is not already purgeable or purged, add it to the
		 * purgeable list.
		 */
		vc4_bo_add_to_purgeable_pool(bo);
	} else if (args->madv == VC4_MADV_WILLNEED &&
		   bo->madv == VC4_MADV_DONTNEED &&
		   !refcount_read(&bo->usecnt)) {
		/* The BO has not been purged yet, just remove it from
		 * the purgeable list.
		 */
		vc4_bo_remove_from_purgeable_pool(bo);
	}

	/* Save the purged state. */
	args->retained = bo->madv != __VC4_MADV_PURGED;

	/* Update internal madv state only if the bo was not purged. */
	if (bo->madv != __VC4_MADV_PURGED)
		bo->madv = args->madv;

	mutex_unlock(&bo->madv_lock);

	ret = 0;

out_put_gem:
	drm_gem_object_put(gem_obj);

	return ret;
}
