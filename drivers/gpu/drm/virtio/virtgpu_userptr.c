// SPDX-License-Identifier: GPL-2.0
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/vmalloc.h>

#include "virtgpu_drv.h"
#include "drm/drm_gem.h"

static struct sg_table *
virtio_gpu_userptr_get_sg_table(struct drm_gem_object *obj);

static void virtio_gpu_userptr_free(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	struct virtio_gpu_object_userptr *userptr = to_virtio_gpu_userptr(bo);

	if (bo->created) {
		userptr->ops->release(userptr);

		virtio_gpu_cmd_unref_resource(vgdev, bo);
		virtio_gpu_notify(vgdev);
	}

	mutex_destroy(&userptr->lock);
}

static void virtio_gpu_userptr_object_close(struct drm_gem_object *obj,
					    struct drm_file *file)
{
	virtio_gpu_gem_object_close(obj, file);
}

static const struct drm_gem_object_funcs virtio_gpu_userptr_funcs = {
	.open = virtio_gpu_gem_object_open,
	.close = virtio_gpu_userptr_object_close,
	.free = virtio_gpu_userptr_free,
	.get_sg_table = virtio_gpu_userptr_get_sg_table,
};

bool virtio_gpu_is_userptr(struct virtio_gpu_object *bo)
{
	return bo->base.base.funcs == &virtio_gpu_userptr_funcs;
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

	if (!(userptr->flags & VIRTGPU_BLOB_FLAG_USERPTR_RDONLY))
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
			return ret;
		}

		pinned += ret;

	} while (pinned < userptr->npages);

	return 0;
}

static void
virtio_gpu_userptr_put_pages(struct virtio_gpu_object_userptr *userptr)
{
	if (userptr->pages) {
		unpin_user_pages(userptr->pages, userptr->npages);
		kvfree(userptr->pages);
		userptr->pages = NULL;
	}

	if (userptr->sgt) {
		sg_free_table(userptr->sgt);
		kfree(userptr->sgt);
		userptr->sgt = NULL;
	}
}

static void
virtio_gpu_userptr_release(struct virtio_gpu_object_userptr *userptr)
{
	mutex_lock(&userptr->lock);
	userptr->ops->put_pages(userptr);
	mutex_unlock(&userptr->lock);
}

static struct sg_table *
virtio_gpu_userptr_get_sg_table(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_object_userptr *userptr = to_virtio_gpu_userptr(bo);
	int ret;

	mutex_lock(&userptr->lock);
	if (!userptr->pages) {
		ret = userptr->ops->get_pages(userptr);
		if (ret) {
			mutex_unlock(&userptr->lock);
			return ERR_PTR(ret);
		}
	}

	if (!userptr->sgt)
		userptr->sgt = drm_prime_pages_to_sg(NULL, userptr->pages,
						     userptr->npages);
	mutex_unlock(&userptr->lock);

	return userptr->sgt;
}

static int
virtio_gpu_userptr_init(struct drm_device *dev, struct drm_file *file,
			struct virtio_gpu_object_userptr *userptr,
			struct virtio_gpu_object_params *params,
			const struct virtio_gpu_object_userptr_ops *ops)
{
	uint32_t page_offset;
	uint64_t aligned_size;
	uint64_t aligned_addr;
	int ret;
	struct drm_gem_object *obj;

	page_offset = params->userptr & (PAGE_SIZE - 1UL);
	aligned_addr = params->userptr - page_offset;
	aligned_size = roundup(page_offset + params->size, PAGE_SIZE);

	userptr->start = aligned_addr;
	userptr->npages = aligned_size >> PAGE_SHIFT;
	userptr->flags = params->blob_flags;

	mutex_init(&userptr->lock);
	userptr->vgdev = dev->dev_private;
	userptr->file = file;
	userptr->ops = ops;

	obj = &userptr->base.base.base;
	obj->funcs = &virtio_gpu_userptr_funcs;

	drm_gem_private_object_init(dev, obj, aligned_size);

	ret = virtio_gpu_resource_id_get(userptr->vgdev,
					 &userptr->base.hw_res_handle);

	return ret;
}

static const struct virtio_gpu_object_userptr_ops virtio_gpu_userptr_ops = {
	.get_pages = virtio_gpu_userptr_get_pages,
	.put_pages = virtio_gpu_userptr_put_pages,
	.release = virtio_gpu_userptr_release,
};

int virtio_gpu_userptr_create(struct virtio_gpu_device *vgdev,
			      struct drm_file *file,
			      struct virtio_gpu_object_params *params,
			      struct virtio_gpu_object **bo_ptr)
{
	struct virtio_gpu_object_userptr *userptr;
	int ret, si;
	struct sg_table *sgt;
	struct scatterlist *sg;
	struct virtio_gpu_mem_entry *ents;

	if (!params->size)
		return -EINVAL;

	if (!access_ok((char __user *)(unsigned long)params->userptr,
		       params->size))
		return -EFAULT;

	userptr = kzalloc(sizeof(*userptr), GFP_KERNEL);
	if (!userptr)
		return -ENOMEM;

	ret = virtio_gpu_userptr_init(vgdev->ddev, file, userptr, params,
				      &virtio_gpu_userptr_ops);
	if (ret)
		goto failed_free;

	sgt = virtio_gpu_userptr_get_sg_table(&userptr->base.base.base);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto failed_free;
	}

	ents = kvmalloc_array(sgt->nents, sizeof(struct virtio_gpu_mem_entry),
			      GFP_KERNEL);
	if (!ents) {
		ret = -ENOMEM;
		goto failed_free;
	}

	for_each_sgtable_sg(sgt, sg, si) {
		(ents)[si].addr = cpu_to_le64(sg_phys(sg));
		(ents)[si].length = cpu_to_le32(sg->length);
		(ents)[si].padding = 0;
	}

	virtio_gpu_cmd_resource_create_blob(vgdev, &userptr->base, params, ents,
					    sgt->nents);

	*bo_ptr = &userptr->base;
	return 0;

failed_free:
	kfree(userptr);
	return ret;
}
