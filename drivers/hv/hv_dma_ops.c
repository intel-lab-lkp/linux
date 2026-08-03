// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Microsoft Corporation.
 *
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/hyperv.h>
#include <linux/smpboot.h>

#include <linux/kernel.h>
#include <linux/dma-map-ops.h>
#include <linux/export.h>
#include <asm/mshyperv.h>
#include "hyperv_vmbus.h"
#include "../../kernel/dma/direct.h"

extern const struct dma_map_ops *dma_ops;

extern bool is_vmbus_dev(struct device *dev);

static bool hyperv_private_memory_dma(struct device *dev)
{
	struct hv_device *hv_dev = device_to_hv_device(dev);

	if (is_vmbus_dev(dev) && hv_dev && hv_dev->channel
	    && hv_dev->channel->co_external_memory)
		return true;

	/* Todo: Check T-Disp capability of PCI device here */

	return false;
}

static int hyperv_dma_map_sg(struct device *dev, struct scatterlist *sgl,
		int nelems, enum dma_data_direction dir,
		unsigned long attrs)
{
	struct scatterlist *sg;
	dma_addr_t dma_addr;
	int i;

	if (hyperv_private_memory_dma(dev)) {
		for_each_sg(sgl, sg, nelems, i) {
			dma_addr = __phys_to_dma(dev, sg_phys(sg));
			sg_dma_address(sg) = dma_addr;
			sg_dma_len(sg) = sg->length;
		}

		return nelems;
	} else {
		return dma_direct_map_sg(dev, sgl, nelems, dir, attrs);
	}
}

static void hyperv_dma_unmap_sg(struct device *dev, struct scatterlist *sgl,
		int nelems, enum dma_data_direction dir, unsigned long attrs)
{
	if (!hyperv_private_memory_dma(dev))
		dma_direct_unmap_sg(dev, sgl, nelems, dir, attrs);
}

static int hyperv_dma_supported(struct device *dev, u64 mask)
{
	dev->coherent_dma_mask = mask;
	return 1;
}

static size_t hyperv_dma_max_mapping_size(struct device *dev)
{
	if (hyperv_private_memory_dma(dev))
		return SIZE_MAX;
	else
		return swiotlb_max_mapping_size(dev);
}

/* allocate and map a coherent mapping */
static void *
hyperv_dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle,
		    gfp_t flag, unsigned long attrs)
{
	phys_addr_t phys;
	void *ret;

	if (!hyperv_private_memory_dma(dev))
		return dma_alloc_coherent(dev, size, dma_handle, flag);

	size = ALIGN(size, PAGE_SIZE);
	ret = (void *)__get_free_pages(flag, get_order(size));
	if (!ret)
		return ret;
	phys = virt_to_phys(ret);

	if (hyperv_private_memory_dma(dev))
		*dma_handle = dma_addr_encrypted(__phys_to_dma(dev, phys));
	else
		*dma_handle = phys_to_dma_unencrypted(dev, phys);

	memset(ret, 0, size);
	return ret;
}

/* free a coherent mapping */
static void
hyperv_dma_free_coherent(struct device *dev, size_t size, void *vaddr,
		   dma_addr_t dma_addr, unsigned long attrs)
{
	if (hyperv_private_memory_dma(dev))
		dmam_free_coherent(dev, size, vaddr, dma_addr);
	else
		free_pages((unsigned long)vaddr, get_order(size));
}

static dma_addr_t hyperv_dma_map_phys(struct device *dev, phys_addr_t phys,
		size_t size, enum dma_data_direction dir,
		unsigned long attrs)
{
	if (hyperv_private_memory_dma(dev))
		return __phys_to_dma(dev, phys);
	else
		return dma_direct_map_phys(dev, phys, size, dir, attrs, true);
}

static void hyperv_dma_unmap_phys(struct device *dev, dma_addr_t dma_handle,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	if (!hyperv_private_memory_dma(dev))
		dma_direct_unmap_phys(dev, dma_handle, size, dir, attrs, true);
}

const struct dma_map_ops hyperv_dma_ops = {
	.alloc			= hyperv_dma_alloc_coherent,
	.free			= hyperv_dma_free_coherent,
	.map_phys               = hyperv_dma_map_phys,
	.unmap_phys             = hyperv_dma_unmap_phys,
	.map_sg                 = hyperv_dma_map_sg,
	.unmap_sg               = hyperv_dma_unmap_sg,
	.dma_supported          = hyperv_dma_supported,
	.max_mapping_size	= hyperv_dma_max_mapping_size,
};
