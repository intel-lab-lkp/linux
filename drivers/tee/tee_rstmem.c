// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Linaro Limited
 */
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/genalloc.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/tee_core.h>
#include "tee_private.h"

struct tee_rstmem_attachment {
	struct sg_table table;
	struct device *dev;
};

static int rstmem_dma_attach(struct dma_buf *dmabuf,
			     struct dma_buf_attachment *attachment)
{
	struct tee_shm *shm = dmabuf->priv;
	struct tee_rstmem_attachment *a;
	int rc;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	if (shm->pages) {
		rc = sg_alloc_table_from_pages(&a->table, shm->pages,
					       shm->num_pages, 0,
					       shm->num_pages * PAGE_SIZE,
					       GFP_KERNEL);
		if (rc)
			goto err;
	} else {
		rc = sg_alloc_table(&a->table, 1, GFP_KERNEL);
		if (rc)
			goto err;
		sg_set_page(a->table.sgl, phys_to_page(shm->paddr), shm->size,
			    0);
	}

	a->dev = attachment->dev;
	attachment->priv = a;

	return 0;
err:
	kfree(a);
	return rc;
}

static void rstmem_dma_detach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attachment)
{
	struct tee_rstmem_attachment *a = attachment->priv;

	sg_free_table(&a->table);
	kfree(a);
}

static struct sg_table *
rstmem_dma_map_dma_buf(struct dma_buf_attachment *attachment,
		       enum dma_data_direction direction)
{
	struct tee_rstmem_attachment *a = attachment->priv;
	int ret;

	ret = dma_map_sgtable(attachment->dev, &a->table, direction,
			      DMA_ATTR_SKIP_CPU_SYNC);
	if (ret)
		return ERR_PTR(ret);

	return &a->table;
}

static void rstmem_dma_unmap_dma_buf(struct dma_buf_attachment *attachment,
				     struct sg_table *table,
				     enum dma_data_direction direction)
{
	struct tee_rstmem_attachment *a = attachment->priv;

	WARN_ON(&a->table != table);

	dma_unmap_sgtable(attachment->dev, table, direction,
			  DMA_ATTR_SKIP_CPU_SYNC);
}

static int rstmem_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
					   enum dma_data_direction direction)
{
	return -EPERM;
}

static int rstmem_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
					 enum dma_data_direction direction)
{
	return -EPERM;
}

static int rstmem_dma_buf_mmap(struct dma_buf *dmabuf,
			       struct vm_area_struct *vma)
{
	return -EPERM;
}

static void rstmem_dma_buf_free(struct dma_buf *dmabuf)
{
	struct tee_shm *shm = dmabuf->priv;

	tee_shm_put(shm);
}

static const struct dma_buf_ops rstmem_generic_buf_ops = {
	.attach = rstmem_dma_attach,
	.detach = rstmem_dma_detach,
	.map_dma_buf = rstmem_dma_map_dma_buf,
	.unmap_dma_buf = rstmem_dma_unmap_dma_buf,
	.begin_cpu_access = rstmem_dma_buf_begin_cpu_access,
	.end_cpu_access = rstmem_dma_buf_end_cpu_access,
	.mmap = rstmem_dma_buf_mmap,
	.release = rstmem_dma_buf_free,
};

struct dma_buf *tee_rstmem_alloc(struct tee_context *ctx, u32 flags,
				 u32 use_case, size_t size, int *shm_id)
{
	struct tee_device *teedev = ctx->teedev;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	struct tee_shm *shm;
	void *ret;
	int rc;

	if (!tee_device_get(teedev))
		return ERR_PTR(-EINVAL);

	if (!teedev->desc->ops->rstmem_alloc ||
	    !teedev->desc->ops->rstmem_free) {
		dmabuf = ERR_PTR(-EINVAL);
		goto err;
	}

	shm = kzalloc(sizeof(*shm), GFP_KERNEL);
	if (!shm) {
		dmabuf = ERR_PTR(-ENOMEM);
		goto err;
	}

	refcount_set(&shm->refcount, 1);
	shm->flags = TEE_SHM_RESTRICTED;
	shm->use_case = use_case;
	shm->ctx = ctx;

	mutex_lock(&teedev->mutex);
	shm->id = idr_alloc(&teedev->idr, NULL, 1, 0, GFP_KERNEL);
	mutex_unlock(&teedev->mutex);
	if (shm->id < 0) {
		dmabuf = ERR_PTR(shm->id);
		goto err_kfree;
	}

	rc = teedev->desc->ops->rstmem_alloc(ctx, shm, flags, use_case, size);
	if (rc) {
		dmabuf = ERR_PTR(rc);
		goto err_idr_remove;
	}

	mutex_lock(&teedev->mutex);
	ret = idr_replace(&teedev->idr, shm, shm->id);
	mutex_unlock(&teedev->mutex);
	if (IS_ERR(ret)) {
		dmabuf = ret;
		goto err_rstmem_free;
	}
	teedev_ctx_get(ctx);

	exp_info.ops = &rstmem_generic_buf_ops;
	exp_info.size = shm->size;
	exp_info.priv = shm;
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		tee_shm_put(shm);
		return dmabuf;
	}

	*shm_id = shm->id;
	return dmabuf;

err_rstmem_free:
	teedev->desc->ops->rstmem_free(ctx, shm);
err_idr_remove:
	mutex_lock(&teedev->mutex);
	idr_remove(&teedev->idr, shm->id);
	mutex_unlock(&teedev->mutex);
err_kfree:
	kfree(shm);
err:
	tee_device_put(teedev);
	return dmabuf;
}
