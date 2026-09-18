// SPDX-License-Identifier: GPL-2.0
#include <linux/dma-mapping.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/pid.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/vmalloc.h>

#include "virtgpu_drv.h"
#include <drm/drm_gem.h>

static void virtio_gpu_userptr_free(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;

	/*
	 * Keep pages pinned until RESOURCE_UNREF completes. The response
	 * callback calls virtio_gpu_cleanup_object(), which drops them.
	 */
	if (bo->created) {
		virtio_gpu_remove_from_restore_list(bo);
		virtio_gpu_cmd_unref_resource(vgdev, bo, false);
		virtio_gpu_notify(vgdev);
		return;
	}

	virtio_gpu_cleanup_object(bo);
}

static struct dma_buf *
virtio_gpu_userptr_prime_export(struct drm_gem_object *obj, int flags)
{
	return ERR_PTR(-EINVAL);
}

static const struct drm_gem_object_funcs virtio_gpu_userptr_funcs = {
	.open = virtio_gpu_gem_object_open,
	.close = virtio_gpu_gem_object_close,
	.free = virtio_gpu_userptr_free,
	.export = virtio_gpu_userptr_prime_export,
};

bool virtio_gpu_is_userptr(struct virtio_gpu_object *bo)
{
	return bo->base.base.funcs == &virtio_gpu_userptr_funcs;
}

void virtio_gpu_userptr_dma_sync_for_device(struct virtio_gpu_object *bo)
{
	struct virtio_gpu_object_userptr *userptr = to_virtio_gpu_userptr(bo);
	struct device *dev;

	if (!userptr->dma_mapped)
		return;

	dev = drm_dev_dma_dev(userptr->base.base.base.dev);
	dma_sync_sgtable_for_device(dev, userptr->sgt, DMA_TO_DEVICE);
}

static int
virtio_gpu_userptr_get_pages(struct virtio_gpu_object_userptr *userptr)
{
	unsigned int flag = FOLL_LONGTERM;
	unsigned int num_pages, pinned = 0;
	int ret = 0;

	if (userptr->pages)
		return 0;

	userptr->pages = kvmalloc_array(userptr->npages, sizeof(struct page *),
					GFP_KERNEL);
	if (!userptr->pages)
		return -ENOMEM;

	if (!(userptr->flags & VIRTGPU_BLOB_FLAG_USE_READONLY))
		flag |= FOLL_WRITE;

	do {
		num_pages = userptr->npages - pinned;

		ret = pin_user_pages_fast(userptr->start + pinned * PAGE_SIZE,
					  num_pages, flag,
					  userptr->pages + pinned);

		if (ret < 0) {
			if (pinned)
				unpin_user_pages(userptr->pages, pinned);
			kvfree(userptr->pages);
			userptr->pages = NULL;
			return ret;
		}

		pinned += ret;

	} while (pinned < userptr->npages);

	return 0;
}

static void
virtio_gpu_userptr_unaccount(struct virtio_gpu_object_userptr *userptr)
{
	if (!userptr->mm)
		return;

	atomic64_sub(userptr->npages, &userptr->mm->pinned_vm);
	mmdrop(userptr->mm);
	userptr->mm = NULL;
}

static void
virtio_gpu_userptr_put_pages(struct virtio_gpu_object_userptr *userptr)
{
	struct drm_device *dev = userptr->base.base.base.dev;

	if (userptr->sgt) {
		if (userptr->dma_mapped)
			dma_unmap_sgtable(drm_dev_dma_dev(dev), userptr->sgt,
					  userptr->dma_dir, 0);
		userptr->dma_mapped = false;
		sg_free_table(userptr->sgt);
		kfree(userptr->sgt);
		userptr->sgt = NULL;
	}

	if (userptr->pages) {
		bool dirty = !(userptr->flags & VIRTGPU_BLOB_FLAG_USE_READONLY);

		unpin_user_pages_dirty_lock(userptr->pages, userptr->npages,
					    dirty);
		kvfree(userptr->pages);
		userptr->pages = NULL;
	}

	virtio_gpu_userptr_unaccount(userptr);
}

static int
virtio_gpu_userptr_get_entries(struct virtio_gpu_device *vgdev,
			       struct virtio_gpu_object_userptr *userptr,
			       struct virtio_gpu_mem_entry **ents,
			       unsigned int *nents)
{
	bool use_dma_api = virtio_gpu_use_dma_api(vgdev->vdev);
	struct scatterlist *sg;
	unsigned int count;
	int si;

	count = use_dma_api ? userptr->sgt->nents : userptr->sgt->orig_nents;
	if (!count)
		return -EINVAL;

	*ents = kvmalloc_array(count, sizeof(**ents), GFP_KERNEL);
	if (!*ents)
		return -ENOMEM;

	if (use_dma_api) {
		for_each_sgtable_dma_sg(userptr->sgt, sg, si) {
			(*ents)[si].addr = cpu_to_le64(sg_dma_address(sg));
			(*ents)[si].length = cpu_to_le32(sg_dma_len(sg));
			(*ents)[si].padding = 0;
		}
	} else {
		for_each_sgtable_sg(userptr->sgt, sg, si) {
			(*ents)[si].addr = cpu_to_le64(sg_phys(sg));
			(*ents)[si].length = cpu_to_le32(sg->length);
			(*ents)[si].padding = 0;
		}
	}

	*nents = count;
	return 0;
}

static int
virtio_gpu_userptr_init(struct drm_device *dev, struct drm_file *file,
			struct virtio_gpu_object_userptr *userptr,
			struct virtio_gpu_object_params *params,
			const struct virtio_gpu_object_userptr_ops *ops)
{
	struct drm_gem_object *obj;
	int ret;

	userptr->start = params->userptr;
	userptr->npages = params->size >> PAGE_SHIFT;
	userptr->flags = params->blob_flags;

	mutex_init(&userptr->lock);
	userptr->vgdev = dev->dev_private;
	userptr->file = file;
	userptr->ops = ops;

	/*
	 * Allocate the resource id before GEM init so a failure here can
	 * unwind with a plain kfree and does not need a special id=0 guard
	 * in the shared resource_id_put helper.
	 */
	ret = virtio_gpu_resource_id_get(userptr->vgdev,
					 &userptr->base.hw_res_handle);
	if (ret) {
		mutex_destroy(&userptr->lock);
		return ret;
	}

	obj = &userptr->base.base.base;
	obj->funcs = &virtio_gpu_userptr_funcs;

	drm_gem_private_object_init(dev, obj, params->size);
	INIT_LIST_HEAD(&userptr->base.restore_node);

	return 0;
}

static const struct virtio_gpu_object_userptr_ops virtio_gpu_userptr_ops = {
	.get_pages = virtio_gpu_userptr_get_pages,
	.put_pages = virtio_gpu_userptr_put_pages,
};

int virtio_gpu_userptr_create(struct virtio_gpu_device *vgdev,
			      struct drm_file *file,
			      struct virtio_gpu_object_params *params,
			      struct virtio_gpu_object **bo_ptr)
{
	struct virtio_gpu_object_userptr *userptr;
	struct virtio_gpu_mem_entry *ents = NULL;
	struct sg_table *sgt;
	struct mm_struct *mm;
	unsigned long lock_limit;
	unsigned long start;
	unsigned long end;
	s64 new_pinned;
	unsigned int nents;
	int ret;

	*bo_ptr = NULL;

	if (!params->size || !IS_ALIGNED(params->size, PAGE_SIZE) ||
	    params->userptr != (unsigned long)params->userptr)
		return -EINVAL;

	start = params->userptr;
	if (!IS_ALIGNED(start, PAGE_SIZE) ||
	    check_add_overflow(start, (unsigned long)params->size, &end))
		return -EINVAL;

	if (!can_do_mlock())
		return -EPERM;

	if (params->size >> PAGE_SHIFT > INT_MAX)
		return -E2BIG;

	if (!access_ok((void __user *)start, params->size))
		return -EFAULT;

	userptr = kzalloc_obj(*userptr);
	if (!userptr)
		return -ENOMEM;

	ret = virtio_gpu_userptr_init(vgdev->ddev, file, userptr, params,
				      &virtio_gpu_userptr_ops);
	if (ret) {
		kfree(userptr);
		return ret;
	}

	mm = current->mm;
	mmgrab(mm);
	lock_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;
	new_pinned = atomic64_add_return(userptr->npages, &mm->pinned_vm);
	if (new_pinned < 0 ||
	    (new_pinned > lock_limit && !capable(CAP_IPC_LOCK))) {
		atomic64_sub(userptr->npages, &mm->pinned_vm);
		mmdrop(mm);
		ret = new_pinned < 0 ? -EOVERFLOW : -ENOMEM;
		goto err_cleanup;
	}
	userptr->mm = mm;

	mutex_lock(&userptr->lock);
	ret = userptr->ops->get_pages(userptr);
	mutex_unlock(&userptr->lock);
	if (ret)
		goto err_cleanup;

	sgt = drm_prime_pages_to_sg(vgdev->ddev, userptr->pages,
				    userptr->npages);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_cleanup;
	}

	userptr->sgt = sgt;

	/*
	 * Match shmem blobs: only DMA-map when the virtio DMA API is in
	 * use. Mapping unconditionally can create SWIOTLB bounce buffers
	 * that get copied back over guest pages on unmap even though the
	 * host was given sg_phys() addresses.
	 */
	if (virtio_gpu_use_dma_api(vgdev->vdev)) {
		enum dma_data_direction dir =
			(userptr->flags & VIRTGPU_BLOB_FLAG_USE_READONLY) ?
			DMA_TO_DEVICE : DMA_BIDIRECTIONAL;

		ret = dma_map_sgtable(drm_dev_dma_dev(vgdev->ddev), sgt,
				      dir, 0);
		if (ret)
			goto err_cleanup;

		userptr->dma_dir = dir;
		userptr->dma_mapped = true;
	}

	ret = virtio_gpu_userptr_get_entries(vgdev, userptr, &ents, &nents);
	if (ret)
		goto err_cleanup;

	virtio_gpu_cmd_resource_create_blob(vgdev, &userptr->base, params, ents,
					    nents);

	*bo_ptr = &userptr->base;
	return 0;

err_cleanup:
	virtio_gpu_cleanup_object(&userptr->base);
	return ret;
}
