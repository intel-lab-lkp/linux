// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include "qda_drv.h"
#include "qda_gem.h"
#include "qda_memory_manager.h"
#include "qda_memory_dma.h"
#include "qda_prime.h"

static int validate_gem_obj_for_mmap(struct qda_gem_obj *qda_gem_obj)
{
	if (qda_gem_obj->size == 0) {
		qda_err(NULL, "Invalid GEM object size\n");
		return -EINVAL;
	}
	if (qda_gem_obj->is_imported) {
		if (!qda_gem_obj->sgt) {
			qda_err(NULL, "Imported buffer missing sgt\n");
			return -EINVAL;
		}
		if (!qda_gem_obj->iommu_dev || !qda_gem_obj->iommu_dev->dev) {
			qda_err(NULL, "Imported buffer missing IOMMU device\n");
			return -EINVAL;
		}
	} else {
		if (!qda_gem_obj->iommu_dev || !qda_gem_obj->iommu_dev->dev) {
			qda_err(NULL, "Allocated buffer missing IOMMU device\n");
			return -EINVAL;
		}
		if (!qda_gem_obj->virt) {
			qda_err(NULL, "Allocated buffer missing virtual address\n");
			return -EINVAL;
		}
		if (qda_gem_obj->dma_addr == 0) {
			qda_err(NULL, "Allocated buffer missing DMA address\n");
			return -EINVAL;
		}
	}
	return 0;
}

static int validate_vma_offset(struct drm_gem_object *drm_obj, struct vm_area_struct *vma)
{
	u64 expected_offset = drm_vma_node_offset_addr(&drm_obj->vma_node);
	u64 actual_offset = vma->vm_pgoff << PAGE_SHIFT;

	if (actual_offset != expected_offset) {
		qda_err(NULL, "VMA offset mismatch: expected=0x%llx, actual=0x%llx\n",
			expected_offset, actual_offset);
		return -EINVAL;
	}

	return 0;
}

static void setup_vma_flags(struct vm_area_struct *vma)
{
	vm_flags_set(vma, VM_DONTEXPAND);
	vm_flags_set(vma, VM_DONTDUMP);
}

void qda_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct qda_gem_obj *qda_gem_obj = to_qda_gem_obj(gem_obj);
	struct qda_drm_priv *drm_priv = get_drm_priv_from_device(gem_obj->dev);

	if (qda_gem_obj->is_imported) {
		if (qda_gem_obj->attachment && qda_gem_obj->sgt)
			dma_buf_unmap_attachment_unlocked(qda_gem_obj->attachment,
							  qda_gem_obj->sgt, DMA_BIDIRECTIONAL);
		if (qda_gem_obj->attachment)
			dma_buf_detach(qda_gem_obj->dma_buf, qda_gem_obj->attachment);
		if (qda_gem_obj->dma_buf)
			dma_buf_put(qda_gem_obj->dma_buf);
		if (qda_gem_obj->iommu_dev && drm_priv && drm_priv->iommu_mgr)
			qda_memory_manager_free(drm_priv->iommu_mgr, qda_gem_obj);
	} else {
		if (qda_gem_obj->virt) {
			if (drm_priv && drm_priv->iommu_mgr)
				qda_memory_manager_free(drm_priv->iommu_mgr, qda_gem_obj);
		}
	}

	drm_gem_object_release(gem_obj);
	kfree(qda_gem_obj);
}

int qda_gem_mmap_obj(struct drm_gem_object *drm_obj, struct vm_area_struct *vma)
{
	struct qda_gem_obj *qda_gem_obj = to_qda_gem_obj(drm_obj);
	int ret;

	ret = validate_gem_obj_for_mmap(qda_gem_obj);
	if (ret) {
		qda_err(NULL, "GEM object validation failed: %d\n", ret);
		return ret;
	}

	ret = validate_vma_offset(drm_obj, vma);
	if (ret) {
		qda_err(NULL, "VMA offset validation failed: %d\n", ret);
		return ret;
	}

	/* Reset vm_pgoff for DMA mmap */
	vma->vm_pgoff = 0;

	ret = qda_dma_mmap(qda_gem_obj, vma);

	if (ret == 0) {
		setup_vma_flags(vma);
		qda_dbg(NULL, "GEM object mapped successfully\n");
	} else {
		qda_err(NULL, "GEM object mmap failed: %d\n", ret);
	}

	return ret;
}

static const struct drm_gem_object_funcs qda_gem_object_funcs = {
	.free = qda_gem_free_object,
	.mmap = qda_gem_mmap_obj,
};

struct qda_gem_obj *qda_gem_alloc_object(struct drm_device *drm_dev, size_t aligned_size)
{
	struct qda_gem_obj *qda_gem_obj;
	int ret;

	qda_gem_obj = kzalloc_obj(*qda_gem_obj, GFP_KERNEL);
	if (!qda_gem_obj)
		return ERR_PTR(-ENOMEM);

	ret = drm_gem_object_init(drm_dev, &qda_gem_obj->base, aligned_size);
	if (ret) {
		qda_err(NULL, "Failed to initialize GEM object: %d\n", ret);
		kfree(qda_gem_obj);
		return ERR_PTR(ret);
	}

	qda_gem_obj->base.funcs = &qda_gem_object_funcs;
	qda_gem_obj->size = aligned_size;

	qda_dbg(NULL, "Allocated GEM object size=%zu\n", aligned_size);
	return qda_gem_obj;
}

void qda_gem_cleanup_object(struct qda_gem_obj *qda_gem_obj)
{
	drm_gem_object_release(&qda_gem_obj->base);
	kfree(qda_gem_obj);
}

struct drm_gem_object *qda_gem_lookup_object(struct drm_file *file_priv, u32 handle)
{
	struct drm_gem_object *gem_obj;

	gem_obj = drm_gem_object_lookup(file_priv, handle);
	if (!gem_obj)
		return ERR_PTR(-ENOENT);

	return gem_obj;
}

int qda_gem_create_handle(struct drm_file *file_priv, struct drm_gem_object *gem_obj, u32 *handle)
{
	int ret;

	ret = drm_gem_handle_create(file_priv, gem_obj, handle);
	drm_gem_object_put(gem_obj);

	return ret;
}

struct drm_gem_object *qda_gem_create_object(struct drm_device *drm_dev,
					     struct qda_memory_manager *iommu_mgr, size_t size,
					     struct drm_file *file_priv)
{
	struct qda_gem_obj *qda_gem_obj;
	size_t aligned_size;
	int ret;

	if (size == 0) {
		qda_err(NULL, "Invalid size for GEM object creation\n");
		return ERR_PTR(-EINVAL);
	}

	aligned_size = PAGE_ALIGN(size);

	qda_gem_obj = qda_gem_alloc_object(drm_dev, aligned_size);
	if (IS_ERR(qda_gem_obj))
		return (struct drm_gem_object *)qda_gem_obj;
	qda_gem_obj->is_imported = false;
	qda_gem_obj->dma_buf = NULL;
	qda_gem_obj->attachment = NULL;
	qda_gem_obj->sgt = NULL;
	qda_gem_obj->imported_dma_addr = 0;

	ret = qda_memory_manager_alloc(iommu_mgr, qda_gem_obj, file_priv);
	if (ret) {
		qda_err(NULL, "Memory manager allocation failed: %d\n", ret);
		qda_gem_cleanup_object(qda_gem_obj);
		return ERR_PTR(ret);
	}

	qda_dbg(NULL, "GEM object created successfully size=%zu\n", aligned_size);
	return &qda_gem_obj->base;
}
