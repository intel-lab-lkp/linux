// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2018, 2020-2021 The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#include <linux/dma-fence.h>
#include <linux/fault-inject.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>

#include "msm_drv.h"
#include "msm_gpu.h"
#include "msm_ioctl.h"

/*
 * DRM ioctls:
 */

static inline ktime_t to_ktime(struct drm_msm_timespec timeout)
{
	return ktime_set(timeout.tv_sec, timeout.tv_nsec);
}

int msm_ioctl_get_param(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_msm_param *args = data;
	struct msm_gpu *gpu;

	/* for now, we just have 3d pipe.. eventually this would need to
	 * be more clever to dispatch to appropriate gpu module:
	 */
	if ((args->pipe != MSM_PIPE_3D0) || (args->pad != 0))
		return -EINVAL;

	gpu = priv->gpu;

	if (!gpu)
		return -ENXIO;

	return gpu->funcs->get_param(gpu, file->driver_priv,
				     args->param, &args->value, &args->len);
}

int msm_ioctl_set_param(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_msm_param *args = data;
	struct msm_gpu *gpu;

	if ((args->pipe != MSM_PIPE_3D0) || (args->pad != 0))
		return -EINVAL;

	gpu = priv->gpu;

	if (!gpu)
		return -ENXIO;

	return gpu->funcs->set_param(gpu, file->driver_priv,
				     args->param, args->value, args->len);
}

int msm_ioctl_gem_new(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_msm_gem_new *args = data;
	uint32_t flags = args->flags;

	if (args->flags & ~MSM_BO_FLAGS) {
		DRM_ERROR("invalid flags: %08x\n", args->flags);
		return -EINVAL;
	}

	/*
	 * Uncached CPU mappings are deprecated, as of:
	 *
	 * 9ef364432db4 ("drm/msm: deprecate MSM_BO_UNCACHED (map as writecombine instead)")
	 *
	 * So promote them to WC.
	 */
	if (flags & MSM_BO_UNCACHED) {
		flags &= ~MSM_BO_CACHED;
		flags |= MSM_BO_WC;
	}

	if (should_fail(&fail_gem_alloc, args->size))
		return -ENOMEM;

	return msm_gem_new_handle(dev, file, args->size,
			args->flags, &args->handle, NULL);
}

int msm_ioctl_gem_cpu_prep(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_msm_gem_cpu_prep *args = data;
	struct drm_gem_object *obj;
	ktime_t timeout = to_ktime(args->timeout);
	int ret;

	if (args->op & ~MSM_PREP_FLAGS) {
		DRM_ERROR("invalid op: %08x\n", args->op);
		return -EINVAL;
	}

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	ret = msm_gem_cpu_prep(obj, args->op, &timeout);

	drm_gem_object_put(obj);

	return ret;
}

int msm_ioctl_gem_cpu_fini(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_msm_gem_cpu_fini *args = data;
	struct drm_gem_object *obj;
	int ret;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	ret = msm_gem_cpu_fini(obj);

	drm_gem_object_put(obj);

	return ret;
}

int msm_ioctl_gem_info_iova(struct drm_device *dev, struct drm_file *file,
			    struct drm_gem_object *obj, uint64_t *iova)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_context *ctx = file->driver_priv;

	if (!priv->gpu)
		return -EINVAL;

	if (msm_context_is_vmbind(ctx))
		return UERR(EINVAL, dev, "VM_BIND is enabled");

	if (should_fail(&fail_gem_iova, obj->size))
		return -ENOMEM;

	/*
	 * Don't pin the memory here - just get an address so that userspace can
	 * be productive
	 */
	return msm_gem_get_iova(obj, msm_context_vm(dev, ctx), iova);
}

int msm_ioctl_gem_info_set_iova(struct drm_device *dev, struct drm_file *file,
				struct drm_gem_object *obj, uint64_t iova)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_context *ctx = file->driver_priv;
	struct drm_gpuvm *vm = msm_context_vm(dev, ctx);

	if (!priv->gpu)
		return -EINVAL;

	if (msm_context_is_vmbind(ctx))
		return UERR(EINVAL, dev, "VM_BIND is enabled");

	/* Only supported if per-process address space is supported: */
	if (priv->gpu->vm == vm)
		return UERR(EOPNOTSUPP, dev, "requires per-process pgtables");

	if (should_fail(&fail_gem_iova, obj->size))
		return -ENOMEM;

	return msm_gem_set_iova(obj, vm, iova);
}

int msm_ioctl_gem_info_set_metadata(struct drm_gem_object *obj,
				    __user void *metadata, u32 metadata_size)
{
	struct msm_gem_object *msm_obj = to_msm_bo(obj);
	void *new_metadata;
	void *buf;
	int ret;

	/* Impose a moderate upper bound on metadata size: */
	if (metadata_size > 128)
		return -EOVERFLOW;

	/* Use a temporary buf to keep copy_from_user() outside of gem obj lock: */
	buf = memdup_user(metadata, metadata_size);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	ret = msm_gem_lock_interruptible(obj);
	if (ret)
		goto out;

	new_metadata =
		krealloc(msm_obj->metadata, metadata_size, GFP_KERNEL);
	if (!new_metadata) {
		ret = -ENOMEM;
		goto out;
	}

	msm_obj->metadata = new_metadata;
	msm_obj->metadata_size = metadata_size;
	memcpy(msm_obj->metadata, buf, metadata_size);

	msm_gem_unlock(obj);

out:
	kfree(buf);

	return ret;
}

int msm_ioctl_gem_info_get_metadata(struct drm_gem_object *obj,
				    __user void *metadata, u32 *metadata_size)
{
	struct msm_gem_object *msm_obj = to_msm_bo(obj);
	void *buf;
	int ret, len;

	if (!metadata) {
		/*
		 * Querying the size is inherently racey, but
		 * EXT_external_objects expects the app to confirm
		 * via device and driver UUIDs that the exporter and
		 * importer versions match.  All we can do from the
		 * kernel side is check the length under obj lock
		 * when userspace tries to retrieve the metadata
		 */
		*metadata_size = msm_obj->metadata_size;
		return 0;
	}

	ret = msm_gem_lock_interruptible(obj);
	if (ret)
		return ret;

	/* Avoid copy_to_user() under gem obj lock: */
	len = msm_obj->metadata_size;
	buf = kmemdup(msm_obj->metadata, len, GFP_KERNEL);

	msm_gem_unlock(obj);

	if (*metadata_size < len)
		ret = -ETOOSMALL;
	else if (copy_to_user(metadata, buf, len))
		ret = -EFAULT;
	else
		*metadata_size = len;

	kfree(buf);

	return 0;
}

int msm_ioctl_gem_info(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_msm_gem_info *args = data;
	struct drm_gem_object *obj;
	struct msm_gem_object *msm_obj;
	int i, ret = 0;

	if (args->pad)
		return -EINVAL;

	switch (args->info) {
	case MSM_INFO_GET_OFFSET:
	case MSM_INFO_GET_IOVA:
	case MSM_INFO_SET_IOVA:
	case MSM_INFO_GET_FLAGS:
		/* value returned as immediate, not pointer, so len==0: */
		if (args->len)
			return -EINVAL;
		break;
	case MSM_INFO_SET_NAME:
	case MSM_INFO_GET_NAME:
	case MSM_INFO_SET_METADATA:
	case MSM_INFO_GET_METADATA:
		break;
	default:
		return -EINVAL;
	}

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	msm_obj = to_msm_bo(obj);

	switch (args->info) {
	case MSM_INFO_GET_OFFSET:
		args->value = msm_gem_mmap_offset(obj);
		break;
	case MSM_INFO_GET_IOVA:
		ret = msm_ioctl_gem_info_iova(dev, file, obj, &args->value);
		break;
	case MSM_INFO_SET_IOVA:
		ret = msm_ioctl_gem_info_set_iova(dev, file, obj, args->value);
		break;
	case MSM_INFO_GET_FLAGS:
		if (drm_gem_is_imported(obj)) {
			ret = -EINVAL;
			break;
		}
		/* Hide internal kernel-only flags: */
		args->value = to_msm_bo(obj)->flags & MSM_BO_FLAGS;
		ret = 0;
		break;
	case MSM_INFO_SET_NAME:
		/* length check should leave room for terminating null: */
		if (args->len >= sizeof(msm_obj->name)) {
			ret = -EINVAL;
			break;
		}
		if (copy_from_user(msm_obj->name, u64_to_user_ptr(args->value),
				   args->len)) {
			msm_obj->name[0] = '\0';
			ret = -EFAULT;
			break;
		}
		msm_obj->name[args->len] = '\0';
		for (i = 0; i < args->len; i++) {
			if (!isprint(msm_obj->name[i])) {
				msm_obj->name[i] = '\0';
				break;
			}
		}
		break;
	case MSM_INFO_GET_NAME:
		if (args->value && (args->len < strlen(msm_obj->name))) {
			ret = -ETOOSMALL;
			break;
		}
		args->len = strlen(msm_obj->name);
		if (args->value) {
			if (copy_to_user(u64_to_user_ptr(args->value),
					 msm_obj->name, args->len))
				ret = -EFAULT;
		}
		break;
	case MSM_INFO_SET_METADATA:
		ret = msm_ioctl_gem_info_set_metadata(
			obj, u64_to_user_ptr(args->value), args->len);
		break;
	case MSM_INFO_GET_METADATA:
		ret = msm_ioctl_gem_info_get_metadata(
			obj, u64_to_user_ptr(args->value), &args->len);
		break;
	}

	drm_gem_object_put(obj);

	return ret;
}

static int wait_fence(struct msm_gpu_submitqueue *queue, uint32_t fence_id,
		      ktime_t timeout, uint32_t flags)
{
	struct dma_fence *fence;
	int ret;

	if (fence_after(fence_id, queue->last_fence)) {
		DRM_ERROR_RATELIMITED("waiting on invalid fence: %u (of %u)\n",
				      fence_id, queue->last_fence);
		return -EINVAL;
	}

	/*
	 * Map submitqueue scoped "seqno" (which is actually an idr key)
	 * back to underlying dma-fence
	 *
	 * The fence is removed from the fence_idr when the submit is
	 * retired, so if the fence is not found it means there is nothing
	 * to wait for
	 */
	spin_lock(&queue->idr_lock);
	fence = idr_find(&queue->fence_idr, fence_id);
	if (fence)
		fence = dma_fence_get_rcu(fence);
	spin_unlock(&queue->idr_lock);

	if (!fence)
		return 0;

	if (flags & MSM_WAIT_FENCE_BOOST)
		dma_fence_set_deadline(fence, ktime_get());

	ret = dma_fence_wait_timeout(fence, true, timeout_to_jiffies(&timeout));
	if (ret == 0)
		ret = -ETIMEDOUT;
	else if (ret != -ERESTARTSYS)
		ret = 0;

	dma_fence_put(fence);

	return ret;
}

int msm_ioctl_wait_fence(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_msm_wait_fence *args = data;
	struct msm_gpu_submitqueue *queue;
	int ret;

	if (args->flags & ~MSM_WAIT_FENCE_FLAGS) {
		DRM_ERROR("invalid flags: %08x\n", args->flags);
		return -EINVAL;
	}

	if (!priv->gpu)
		return 0;

	queue = msm_submitqueue_get(file->driver_priv, args->queueid);
	if (!queue)
		return -ENOENT;

	ret = wait_fence(queue, args->fence, to_ktime(args->timeout), args->flags);

	msm_submitqueue_put(queue);

	return ret;
}

int msm_ioctl_gem_madvise(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_msm_gem_madvise *args = data;
	struct drm_gem_object *obj;
	int ret;

	switch (args->madv) {
	case MSM_MADV_DONTNEED:
	case MSM_MADV_WILLNEED:
		break;
	default:
		return -EINVAL;
	}

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	ret = msm_gem_madvise(obj, args->madv);
	if (ret >= 0) {
		args->retained = ret;
		ret = 0;
	}

	drm_gem_object_put(obj);

	return ret;
}

int msm_ioctl_submitqueue_new(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_msm_submitqueue *args = data;

	if (args->flags & ~MSM_SUBMITQUEUE_FLAGS)
		return -EINVAL;

	return msm_submitqueue_create(dev, file->driver_priv, args->prio,
		args->flags, &args->id);
}

int msm_ioctl_submitqueue_query(struct drm_device *dev, void *data, struct drm_file *file)
{
	return msm_submitqueue_query(dev, file->driver_priv, data);
}

int msm_ioctl_submitqueue_close(struct drm_device *dev, void *data, struct drm_file *file)
{
	u32 id = *(u32 *) data;

	return msm_submitqueue_remove(file->driver_priv, id);
}
