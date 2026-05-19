// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include "qda_drv.h"
#include "qda_gem.h"
#include "qda_memory_dma.h"

static dma_addr_t get_actual_dma_addr(struct qda_gem_obj *gem_obj)
{
	return gem_obj->dma_addr - ((u64)gem_obj->iommu_dev->sid << 32);
}

static void setup_gem_object(struct qda_gem_obj *gem_obj, void *virt,
			     dma_addr_t dma_addr, struct qda_iommu_device *iommu_dev)
{
	gem_obj->virt = virt;
	gem_obj->dma_addr = dma_addr;
	gem_obj->iommu_dev = iommu_dev;
}

static void cleanup_gem_object_fields(struct qda_gem_obj *gem_obj)
{
	gem_obj->virt = NULL;
	gem_obj->dma_addr = 0;
	gem_obj->iommu_dev = NULL;
}

/**
 * qda_dma_alloc() - Allocate DMA coherent memory for a GEM object
 * @iommu_dev: Pointer to the QDA IOMMU device structure
 * @gem_obj: Pointer to GEM object to allocate memory for
 * @size: Size of memory to allocate in bytes
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_dma_alloc(struct qda_iommu_device *iommu_dev,
		  struct qda_gem_obj *gem_obj, size_t size)
{
	void *virt;
	dma_addr_t dma_addr;

	if (!iommu_dev || !iommu_dev->dev) {
		pr_err("qda: Invalid iommu_dev or device for DMA allocation\n");
		return -EINVAL;
	}

	virt = dma_alloc_coherent(iommu_dev->dev, size, &dma_addr, GFP_KERNEL);
	if (!virt)
		return -ENOMEM;

	dma_addr += ((u64)iommu_dev->sid << 32);

	dev_dbg(iommu_dev->dev, "DMA address with SID prefix: 0x%llx (sid=%u)\n",
		(u64)dma_addr, iommu_dev->sid);

	setup_gem_object(gem_obj, virt, dma_addr, iommu_dev);

	return 0;
}

/**
 * qda_dma_free() - Free DMA coherent memory for a GEM object
 * @gem_obj: Pointer to GEM object to free memory for
 */
void qda_dma_free(struct qda_gem_obj *gem_obj)
{
	if (!gem_obj || !gem_obj->iommu_dev) {
		pr_debug("qda: Invalid gem_obj or iommu_dev for DMA free\n");
		return;
	}

	dev_dbg(gem_obj->iommu_dev->dev, "DMA freeing: size=%zu, device_id=%u, dma_addr=0x%llx\n",
		gem_obj->size, gem_obj->iommu_dev->id, gem_obj->dma_addr);

	dma_free_coherent(gem_obj->iommu_dev->dev, gem_obj->size,
			  gem_obj->virt, get_actual_dma_addr(gem_obj));

	cleanup_gem_object_fields(gem_obj);
}

/**
 * qda_dma_mmap() - Map DMA memory into userspace
 * @gem_obj: Pointer to GEM object containing DMA memory
 * @vma: Virtual memory area to map into
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_dma_mmap(struct qda_gem_obj *gem_obj, struct vm_area_struct *vma)
{
	struct qda_iommu_device *iommu_dev;
	int ret;

	if (!gem_obj || !gem_obj->virt || !gem_obj->iommu_dev || !gem_obj->iommu_dev->dev) {
		pr_err("qda: Invalid parameters for DMA mmap\n");
		return -EINVAL;
	}

	iommu_dev = gem_obj->iommu_dev;

	ret = dma_mmap_coherent(iommu_dev->dev, vma, gem_obj->virt,
				get_actual_dma_addr(gem_obj), gem_obj->size);
	if (ret) {
		dev_err(iommu_dev->dev, "DMA mmap failed: size=%zu, device_id=%u, ret=%d\n",
			gem_obj->size, iommu_dev->id, ret);
		return ret;
	}

	return 0;
}
