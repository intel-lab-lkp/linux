// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 Intel Corporation. All rights reserved. */
#include <linux/module.h>
#include <linux/dax.h>

#include "../cxl/cxl.h"
#include "bus.h"
#include "dax-private.h"

static int __cxl_dax_add_resource(struct dax_region *dax_region,
				  struct dc_extent *dc_extent)
{
	struct device *dev = &dc_extent->dev;
	resource_size_t start, length;

	start = dax_region->res.start + dc_extent->hpa_range.start;
	length = range_len(&dc_extent->hpa_range);
	return dax_region_add_resource(dax_region, dev, start, length,
				       &dc_extent->group->uuid,
				       dc_extent->seq_num);
}

static int cxl_dax_add_resource(struct device *dev, void *data)
{
	struct dax_region *dax_region = data;
	struct dc_extent *dc_extent;

	dc_extent = to_dc_extent(dev);
	if (!dc_extent)
		return 0;

	dev_dbg(dax_region->dev, "Adding resource HPA %pra (%pUb)\n",
		&dc_extent->hpa_range, &dc_extent->group->uuid);

	return __cxl_dax_add_resource(dax_region, dc_extent);
}

static int cxl_dax_group_add(struct dax_region *dax_region,
			     struct cxl_dc_tag_group *group)
{
	struct dc_extent *dc_extent;
	unsigned long index;
	int rc;

	xa_for_each(&group->dc_extents, index, dc_extent) {
		rc = __cxl_dax_add_resource(dax_region, dc_extent);
		if (rc)
			return rc;
	}
	return 0;
}

/*
 * RELEASE is still a stub here — the atomic dax_region_rm_resources API
 * and its wire-up land in the next commit.  An incoming RELEASE returns
 * success and the cxl side proceeds to rm_tag_group(), which device-
 * unregisters each dc_extent; the devm action armed by
 * dax_region_add_resource() then tears down each dax_resource.
 */
static int cxl_dax_region_notify(struct device *dev,
				 struct cxl_notify_data *notify_data)
{
	struct cxl_dax_region *cxlr_dax = to_cxl_dax_region(dev);
	struct dax_region *dax_region = dev_get_drvdata(dev);
	struct cxl_dc_tag_group *group = notify_data->group;

	switch (notify_data->event) {
	case DCD_ADD_CAPACITY:
		return cxl_dax_group_add(dax_region, group);
	case DCD_RELEASE_CAPACITY:
		dev_dbg(&cxlr_dax->dev,
			"DCD RELEASE notify (tag %pUb): no-op (stub)\n",
			&group->uuid);
		return 0;
	case DCD_FORCED_CAPACITY_RELEASE:
	default:
		dev_err(&cxlr_dax->dev, "Unknown DC event %d\n",
			notify_data->event);
		return -ENXIO;
	}
}

static struct dax_dc_ops dc_ops = {
	.is_extent = is_dc_extent,
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

	if (cxlr->mode == CXL_PARTMODE_DYNAMIC_RAM_A)
		flags = IORESOURCE_DAX_DCD;
	else
		flags = IORESOURCE_DAX_KMEM;

	dax_region = alloc_dax_region(dev, cxlr->id, &cxlr_dax->hpa_range, nid,
				      PMD_SIZE, flags, &dc_ops);
	if (!dax_region)
		return -ENOMEM;

	if (cxlr->mode == CXL_PARTMODE_DYNAMIC_RAM_A) {
		device_for_each_child(&cxlr_dax->dev, dax_region,
				      cxl_dax_add_resource);
		/* Add empty seed dax device */
		dev_size = 0;
	} else {
		dev_size = range_len(&cxlr_dax->hpa_range);
	}

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
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

static void cxl_dax_region_driver_register(struct work_struct *work)
{
	dax_hmem_flush_work();
	cxl_driver_register(&cxl_dax_region_driver);
}

static DECLARE_WORK(cxl_dax_region_driver_work, cxl_dax_region_driver_register);

static int __init cxl_dax_region_init(void)
{
	/*
	 * Need to resolve a race with dax_hmem wanting to drive regions
	 * instead of CXL
	 */
	queue_work(system_long_wq, &cxl_dax_region_driver_work);
	return 0;
}
module_init(cxl_dax_region_init);

static void __exit cxl_dax_region_exit(void)
{
	flush_work(&cxl_dax_region_driver_work);
	cxl_driver_unregister(&cxl_dax_region_driver);
}
module_exit(cxl_dax_region_exit);

MODULE_ALIAS_CXL(CXL_DEVICE_DAX_REGION);
MODULE_DESCRIPTION("CXL DAX: direct access to CXL regions");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel Corporation");
MODULE_IMPORT_NS("CXL");
