// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include "qda_drv.h"
#include "qda_gem.h"
#include "qda_prime.h"
#include "qda_memory_manager.h"

static struct drm_gem_object *check_own_buffer(struct drm_device *dev, struct dma_buf *dma_buf)
{
	struct drm_gem_object *existing_gem;

	/* Only safe to access priv if this dma-buf was exported by this device */
	if (!drm_gem_is_prime_exported_dma_buf(dev, dma_buf))
		return NULL;

	existing_gem = dma_buf->priv;
	if (existing_gem->dev != dev)
		return NULL;

	if (to_qda_gem_obj(existing_gem)->is_imported)
		return NULL;

	drm_gem_object_get(existing_gem);
	return existing_gem;
}

static struct qda_iommu_device *get_iommu_device_for_import(struct qda_dev *qdev,
							    struct drm_file **file_priv_out)
{
	struct drm_file *file_priv;
	struct qda_file_priv *qda_file_priv;
	struct qda_iommu_device *iommu_dev = NULL;
	int ret;

	file_priv = qdev->current_import_file_priv;
	*file_priv_out = file_priv;

	if (!file_priv || !file_priv->driver_priv)
		return NULL;

	qda_file_priv = (struct qda_file_priv *)file_priv->driver_priv;
	iommu_dev = qda_file_priv->assigned_iommu_dev;

	if (!iommu_dev) {
		ret = qda_memory_manager_assign_device(qdev->iommu_mgr, file_priv);
		if (ret) {
			drm_err(&qdev->drm_dev, "Failed to assign IOMMU device: %d\n", ret);
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
		drm_err(&qdev->drm_dev, "Failed to attach dma_buf: %d\n", ret);
		return ret;
	}
	qda_gem_obj->attachment = attachment;

	sgt = dma_buf_map_attachment_unlocked(attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		drm_err(&qdev->drm_dev, "Failed to map dma_buf attachment: %d\n", ret);
		dma_buf_detach(dma_buf, attachment);
		return ret;
	}
	qda_gem_obj->sgt = sgt;

	return 0;
}

/**
 * qda_gem_prime_import() - Import a DMA-BUF as a GEM object
 * @dev: DRM device structure
 * @dma_buf: DMA-BUF to import
 *
 * Return: Pointer to the imported GEM object on success, ERR_PTR on failure
 */
struct drm_gem_object *qda_gem_prime_import(struct drm_device *dev, struct dma_buf *dma_buf)
{
	struct qda_dev *qdev = qda_dev_from_drm(dev);
	struct qda_gem_obj *qda_gem_obj;
	struct drm_file *file_priv;
	struct qda_iommu_device *iommu_dev;
	struct drm_gem_object *existing_gem;
	size_t aligned_size;
	int ret;

	if (!qdev->iommu_mgr) {
		drm_err(dev, "Invalid iommu_mgr\n");
		return ERR_PTR(-ENODEV);
	}

	existing_gem = check_own_buffer(dev, dma_buf);
	if (existing_gem)
		return existing_gem;

	iommu_dev = get_iommu_device_for_import(qdev, &file_priv);
	if (!iommu_dev || !iommu_dev->dev) {
		drm_err(dev, "No IOMMU device assigned for prime import\n");
		return ERR_PTR(-ENODEV);
	}

	drm_dbg_driver(dev, "Using IOMMU device %u for prime import\n", iommu_dev->id);

	aligned_size = PAGE_ALIGN(dma_buf->size);
	qda_gem_obj = qda_gem_alloc_object(dev, aligned_size);
	if (IS_ERR(qda_gem_obj))
		return ERR_CAST(qda_gem_obj);

	qda_gem_obj->is_imported = true;
	qda_gem_obj->dma_buf = dma_buf;
	qda_gem_obj->virt = NULL;
	qda_gem_obj->iommu_dev = iommu_dev;

	get_dma_buf(dma_buf);

	ret = setup_dma_buf_mapping(qda_gem_obj, dma_buf, iommu_dev->dev, qdev);
	if (ret)
		goto err_put_dma_buf;

	ret = qda_memory_manager_alloc(qdev->iommu_mgr, qda_gem_obj, file_priv);
	if (ret) {
		drm_err(dev, "Failed to allocate IOMMU mapping: %d\n", ret);
		goto err_unmap;
	}

	drm_dbg_driver(dev, "Prime import completed successfully size=%zu\n", aligned_size);
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

/**
 * qda_prime_fd_to_handle() - Convert a PRIME fd to a GEM handle
 * @dev: DRM device structure
 * @file_priv: DRM file private data
 * @prime_fd: File descriptor of the PRIME buffer
 * @handle: Output GEM handle
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv,
			   int prime_fd, u32 *handle)
{
	struct qda_dev *qdev = qda_dev_from_drm(dev);
	int ret;

	mutex_lock(&qdev->import_lock);
	qdev->current_import_file_priv = file_priv;

	ret = drm_gem_prime_fd_to_handle(dev, file_priv, prime_fd, handle);

	qdev->current_import_file_priv = NULL;
	mutex_unlock(&qdev->import_lock);

	return ret;
}

MODULE_IMPORT_NS("DMA_BUF");
