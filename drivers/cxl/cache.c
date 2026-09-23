// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 Advanced Micro Devices, Inc. */

#include <cxl/cxl.h>

#include "cxlcache.h"

/**
 * DOC: cxl cache
 *
 * The cxl_cache driver is responsible for validating the CXL.cache system
 * configuration and providing a common management interface for the CXL cache
 * of CXL.cache enabled devices. This driver does not discover devices; a
 * device-specific driver is required for discovery and portions of set up.
 */

/**
 * devm_cxl_add_cachedev - Add a CXL cache device
 * @cxlds: CXL device state to associate with the cachedev
 */
struct cxl_cachedev *devm_cxl_add_cachedev(struct cxl_dev_state *cxlds)
{
	return __devm_cxl_add_cachedev(cxlds, NULL);
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_add_cachedev, "CXL");

static int cxl_cache_probe(struct device *dev)
{
	struct cxl_cachedev *cxlcd = to_cxl_cachedev(dev);
	struct cxl_dev_state *cxlds = cxlcd->cxlds;
	struct device *endpoint_parent;
	struct cxl_dport *dport;
	int rc;

	/* Disable CXL.cache until we can validate the device configuration */
	cxl_clear_cache_enable(cxlds);

	/* See comment in cxl_mem_probe() */
	if (work_pending(&cxlcd->detach_work))
		return -EBUSY;

	rc = cxl_accel_read_cache_info(cxlds);
	if (rc)
		return rc;

	rc = devm_cxl_enumerate_ports(&cxlcd->dev);
	if (rc)
		return rc;

	struct cxl_port *parent_port __free(put_cxl_port) =
		cxl_cache_find_port(cxlcd, &dport);
	if (!parent_port) {
		dev_err(dev, "CXL port topology not found\n");
		return -ENXIO;
	}

	if (dport->rch)
		endpoint_parent = parent_port->uport_dev;
	else
		endpoint_parent = &parent_port->dev;

	scoped_guard(device, endpoint_parent) {
		if (!endpoint_parent->driver) {
			dev_err(dev, "CXL port topology %s not enabled\n",
				dev_name(endpoint_parent));
			return -ENXIO;
		}

		rc = devm_cxl_add_endpoint(endpoint_parent, &cxlcd->dev, dport);
		if (rc)
			return rc;
	}

	return 0;
}

static struct cxl_driver cxl_cache_driver = {
	.name = "cxl_cache",
	.probe = cxl_cache_probe,
	.drv = {
		/*
		 * Needed to guarantee probe and set up order for
		 * endpoint drivers.
		 */
		.probe_type = PROBE_FORCE_SYNCHRONOUS,
	},
	.id = CXL_DEVICE_ACCELERATOR,
};

module_cxl_driver(cxl_cache_driver);

MODULE_DESCRIPTION("CXL: Cache Management");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("CXL");
MODULE_ALIAS_CXL(CXL_DEVICE_ACCELERATOR);
