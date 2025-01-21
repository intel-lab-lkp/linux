// SPDX-License-Identifier: GPL-2.0
/*
 * dma_timer.c -- Freescale ColdFire DMA Ops helpers.
 *
 * Copyright (C) 2024 Jean-Michel Hautbois, Yoseli
 *
 */

#include <linux/device.h>
#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/pci.h>
#include <asm/pgalloc.h>

static void *coldfire_alloc_coherent(struct device *dev, size_t size,
				     dma_addr_t *dma_handle, gfp_t gfp,
				     unsigned long attrs)
{
	void *vaddr;

	vaddr = dma_alloc_from_global_coherent(dev, size, dma_handle);
	if (!vaddr) {
		dev_err(dev, "Failed to allocate %zu bytes\n", size);
		return NULL;
	}

	dev_dbg(dev, "Allocated %zu bytes at %p (dma %pad)\n", size, vaddr, dma_handle);

	return vaddr;
}

static void coldfire_free_coherent(struct device *dev, size_t size, void *vaddr,
				   dma_addr_t dma_handle, unsigned long attrs)
{
	unsigned int page_order = get_order(size);

	if (!dma_release_from_global_coherent(page_order, vaddr))
		WARN_ON_ONCE(1);
}

static dma_addr_t coldfire_map_page(struct device *dev, struct page *page,
				    unsigned long offset, size_t size,
				    enum dma_data_direction dir, unsigned long attrs)
{
	phys_addr_t phys = page_to_phys(page) + offset;
	dma_addr_t dma_addr = phys_to_dma(dev, phys);

	if (!dev_is_dma_coherent(dev) && !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		arch_sync_dma_for_device(phys, size, dir);
	return dma_addr;
}

static void coldfire_unmap_page(struct device *dev, dma_addr_t dma_handle,
				size_t size, enum dma_data_direction dir,
				unsigned long attrs)
{
	phys_addr_t phys = dma_to_phys(dev, dma_handle);

	if (!dev_is_dma_coherent(dev) && !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		arch_sync_dma_for_cpu(phys, size, dir);
}

static int coldfire_map_sg(struct device *dev, struct scatterlist *sg,
			   int nents, enum dma_data_direction dir,
			   unsigned long attrs)
{
	struct scatterlist *s;
	dma_addr_t dma_handle;
	int i;

	if (!sg) {
		dev_err(dev, "Invalid scatterlist, cannot map memory\n");
		return 0;
	}

	for_each_sg(sg, s, nents, i) {
		dma_handle = page_to_phys(sg_page(s)) + sg->offset;
		dev_dbg(dev, "Mapped scatterlist %p (offset %u) to DMA address %pad\n",
			sg_page(s), sg->offset, &dma_handle);
	}

	return nents;
}

static void coldfire_unmap_sg(struct device *dev, struct scatterlist *sg,
			      int nents, enum dma_data_direction dir,
			      unsigned long attrs)
{
}

static dma_addr_t coldfire_map_resource(struct device *dev, phys_addr_t phys,
					size_t size, enum dma_data_direction dir,
					unsigned long attrs)
{
	dma_addr_t dma_handle;

	if (!phys) {
		dev_err(dev, "Invalid physical address, cannot map memory\n");
		return 0;
	}

	dma_handle = phys;
	dev_dbg(dev, "Mapped physical address %pa (size %zu) to DMA address %pad\n",
		&phys, size, &dma_handle);
	return dma_handle;
}

static void coldfire_unmap_resource(struct device *dev, dma_addr_t dma_handle,
				    size_t size, enum dma_data_direction dir,
				    unsigned long attrs)
{
}

const struct dma_map_ops coldfire_dma_ops = {
	.alloc			= coldfire_alloc_coherent,
	.free			= coldfire_free_coherent,
	.map_page		= coldfire_map_page,
	.unmap_page		= coldfire_unmap_page,
	.map_sg			= coldfire_map_sg,
	.unmap_sg		= coldfire_unmap_sg,
	.map_resource		= coldfire_map_resource,
	.unmap_resource		= coldfire_unmap_resource,
	.alloc_pages_op		= dma_common_alloc_pages,
	.free_pages		= dma_common_free_pages,
};
EXPORT_SYMBOL(coldfire_dma_ops);
