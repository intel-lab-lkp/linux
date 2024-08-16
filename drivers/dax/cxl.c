// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. All rights reserved. */
#include <linux/module.h>
#include <linux/dax.h>

#include "../cxl/cxl.h"
#include "bus.h"
#include "dax-private.h"

static int __cxl_dax_add_resource(struct dax_region *dax_region,
				  struct region_extent *region_extent)
{
	resource_size_t start, length;
	struct device *dev;

	dev = &region_extent->dev;
	start = dax_region->res.start + region_extent->hpa_range.start;
	length = range_len(&region_extent->hpa_range);
	return dax_region_add_resource(dax_region, dev, start, length);
}

static int cxl_dax_add_resource(struct device *dev, void *data)
{
	struct dax_region *dax_region = data;
	struct region_extent *region_extent;

	region_extent = to_region_extent(dev);
	if (!region_extent)
		return 0;

	dev_dbg(dax_region->dev, "Adding resource HPA %par\n",
		&region_extent->hpa_range);

	return __cxl_dax_add_resource(dax_region, region_extent);
}

static int cxl_dax_region_notify(struct device *dev,
				 struct cxl_notify_data *notify_data)
{
	struct cxl_dax_region *cxlr_dax = to_cxl_dax_region(dev);
	struct dax_region *dax_region = dev_get_drvdata(dev);
	struct region_extent *region_extent = notify_data->region_extent;

	switch (notify_data->event) {
	case DCD_ADD_CAPACITY:
		return __cxl_dax_add_resource(dax_region, region_extent);
	case DCD_RELEASE_CAPACITY:
		return dax_region_rm_resource(dax_region, &region_extent->dev);
	case DCD_FORCED_CAPACITY_RELEASE:
	default:
		dev_err(&cxlr_dax->dev, "Unknown DC event %d\n",
			notify_data->event);
		break;
	}

	return -ENXIO;
}

struct dax_sparse_ops sparse_ops = {
	.is_extent = is_region_extent,
};

static int cxl_dax_region_probe(struct device *dev)
{
	struct cxl_dax_region *cxlr_dax = to_cxl_dax_region(dev);
	int nid = phys_to_target_node(cxlr_dax->hpa_range.start);
	struct cxl_region *cxlr = cxlr_dax->cxlr;
	struct dax_region *dax_region;
	struct dev_dax_data data;
	resource_size_t dev_size;
	unsigned long flags;

	if (nid == NUMA_NO_NODE)
		nid = memory_add_physaddr_to_nid(cxlr_dax->hpa_range.start);

	flags = IORESOURCE_DAX_KMEM;
	if (cxlr->mode == CXL_REGION_DC)
		flags |= IORESOURCE_DAX_SPARSE_CAP;

	dax_region = alloc_dax_region(dev, cxlr->id, &cxlr_dax->hpa_range, nid,
				      PMD_SIZE, flags, &sparse_ops);
	if (!dax_region)
		return -ENOMEM;

	if (cxlr->mode == CXL_REGION_DC) {
		device_for_each_child(&cxlr_dax->dev, dax_region,
				      cxl_dax_add_resource);
		/* Add empty seed dax device */
		dev_size = 0;
	} else
		dev_size = range_len(&cxlr_dax->hpa_range);

	data = (struct dev_dax_data) {
		.dax_region = dax_region,
		.id = -1,
		.size = dev_size,
		.memmap_on_memory = true,
	};

	return PTR_ERR_OR_ZERO(devm_create_dev_dax(&data));
}

static struct cxl_driver cxl_dax_region_driver = {
	.name = "cxl_dax_region",
	.probe = cxl_dax_region_probe,
	.notify = cxl_dax_region_notify,
	.id = CXL_DEVICE_DAX_REGION,
	.drv = {
		.suppress_bind_attrs = true,
	},
};

module_cxl_driver(cxl_dax_region_driver);
MODULE_ALIAS_CXL(CXL_DEVICE_DAX_REGION);
MODULE_DESCRIPTION("CXL DAX: direct access to CXL regions");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel Corporation");
MODULE_IMPORT_NS(CXL);
