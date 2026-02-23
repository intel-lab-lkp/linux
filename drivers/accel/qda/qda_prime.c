// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include "qda_drv.h"
#include "qda_gem.h"
#include "qda_prime.h"
#include "qda_memory_manager.h"

static struct drm_gem_object *check_own_buffer(struct drm_device *dev, struct dma_buf *dma_buf)
{
	if (dma_buf->priv) {
		struct drm_gem_object *existing_gem = dma_buf->priv;

		if (existing_gem->dev == dev) {
			struct qda_gem_obj *existing_qda_gem = to_qda_gem_obj(existing_gem);

			if (!existing_qda_gem->is_imported) {
				drm_gem_object_get(existing_gem);
				return existing_gem;
			}
		}
	}
	return NULL;
}

static struct qda_iommu_device *get_iommu_device_for_import(struct qda_drm_priv *drm_priv,
							    struct drm_file **file_priv_out,
							    struct qda_dev *qdev)
{
	struct drm_file *file_priv;
	struct qda_file_priv *qda_file_priv;
	struct qda_iommu_device *iommu_dev = NULL;
	int ret;

	file_priv = drm_priv->current_import_file_priv;
	*file_priv_out = file_priv;

	if (!file_priv || !file_priv->driver_priv)
		return NULL;

	qda_file_priv = (struct qda_file_priv *)file_priv->driver_priv;
	iommu_dev = qda_file_priv->assigned_iommu_dev;

	if (!iommu_dev) {
		ret = qda_memory_manager_assign_device(drm_priv->iommu_mgr, file_priv);
		if (ret) {
			qda_err(qdev, "Failed to assign IOMMU device: %d\n", ret);
			return NULL;
		}

		iommu_dev = qda_file_priv->assigned_iommu_dev;
	}

	return iommu_dev;
}

static int setup_dma_buf_mapping(struct qda_gem_obj *qda_gem_obj, struct dma_buf *dma_buf,
				 struct device *attach_dev, struct qda_dev *qdev)
{
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt;
	int ret;

	attachment = dma_buf_attach(dma_buf, attach_dev);
	if (IS_ERR(attachment)) {
		ret = PTR_ERR(attachment);
		qda_err(qdev, "Failed to attach dma_buf: %d\n", ret);
		return ret;
	}
	qda_gem_obj->attachment = attachment;

	sgt = dma_buf_map_attachment_unlocked(attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		qda_err(qdev, "Failed to map dma_buf attachment: %d\n", ret);
		dma_buf_detach(dma_buf, attachment);
		return ret;
	}
	qda_gem_obj->sgt = sgt;

	return 0;
}

struct drm_gem_object *qda_gem_prime_import(struct drm_device *dev, struct dma_buf *dma_buf)
{
	struct qda_drm_priv *drm_priv;
	struct qda_gem_obj *qda_gem_obj;
	struct drm_file *file_priv;
	struct qda_iommu_device *iommu_dev;
	struct qda_dev *qdev;
	struct drm_gem_object *existing_gem;
	size_t aligned_size;
	int ret;

	drm_priv = get_drm_priv_from_device(dev);
	if (!drm_priv || !drm_priv->iommu_mgr) {
		qda_err(NULL, "Invalid drm_priv or iommu_mgr\n");
		return ERR_PTR(-EINVAL);
	}

	qdev = drm_priv->qdev;

	existing_gem = check_own_buffer(dev, dma_buf);
	if (existing_gem)
		return existing_gem;

	iommu_dev = get_iommu_device_for_import(drm_priv, &file_priv, qdev);
	if (!iommu_dev || !iommu_dev->dev) {
		qda_err(qdev, "No IOMMU device assigned for prime import\n");
		return ERR_PTR(-ENODEV);
	}

	qda_dbg(qdev, "Using IOMMU device %u for prime import\n", iommu_dev->id);

	aligned_size = PAGE_ALIGN(dma_buf->size);
	qda_gem_obj = qda_gem_alloc_object(dev, aligned_size);
	if (IS_ERR(qda_gem_obj))
		return (struct drm_gem_object *)qda_gem_obj;

	qda_gem_obj->is_imported = true;
	qda_gem_obj->dma_buf = dma_buf;
	qda_gem_obj->virt = NULL;
	qda_gem_obj->dma_addr = 0;
	qda_gem_obj->imported_dma_addr = 0;
	qda_gem_obj->iommu_dev = iommu_dev;

	get_dma_buf(dma_buf);

	ret = setup_dma_buf_mapping(qda_gem_obj, dma_buf, iommu_dev->dev, qdev);
	if (ret)
		goto err_put_dma_buf;

	ret = qda_memory_manager_alloc(drm_priv->iommu_mgr, qda_gem_obj, file_priv);
	if (ret) {
		qda_err(qdev, "Failed to allocate IOMMU mapping: %d\n", ret);
		goto err_unmap;
	}

	qda_dbg(qdev, "Prime import completed successfully size=%zu\n", aligned_size);
	return &qda_gem_obj->base;

err_unmap:
	dma_buf_unmap_attachment_unlocked(qda_gem_obj->attachment,
					  qda_gem_obj->sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(dma_buf, qda_gem_obj->attachment);
err_put_dma_buf:
	dma_buf_put(dma_buf);
	qda_gem_cleanup_object(qda_gem_obj);
	return ERR_PTR(ret);
}

int qda_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv,
			   int prime_fd, u32 *handle)
{
	struct qda_drm_priv *drm_priv;
	struct qda_dev *qdev;
	int ret;

	drm_priv = get_drm_priv_from_device(dev);
	if (!drm_priv) {
		qda_dbg(NULL, "Failed to get drm_priv from device\n");
		return -EINVAL;
	}

	qdev = drm_priv->qdev;

	if (file_priv && file_priv->driver_priv) {
		struct qda_file_priv *qda_file_priv;

		qda_file_priv = (struct qda_file_priv *)file_priv->driver_priv;
	} else {
		qda_dbg(qdev, "Called with NULL file_priv or driver_priv\n");
	}

	mutex_lock(&drm_priv->import_lock);
	drm_priv->current_import_file_priv = file_priv;

	ret = drm_gem_prime_fd_to_handle(dev, file_priv, prime_fd, handle);

	drm_priv->current_import_file_priv = NULL;
	mutex_unlock(&drm_priv->import_lock);

	if (!ret)
		qda_dbg(qdev, "Completed with ret=%d, handle=%u\n", ret, *handle);
	else
		qda_dbg(qdev, "Completed with ret=%d\n", ret);

	return ret;
}

MODULE_IMPORT_NS("DMA_BUF");
