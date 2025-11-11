// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2025 Advanced Micro Devices, Inc. */
#include "cxlcache.h"
#include "private.h"

/**
 * DOC: cxl cache
 *
 * The cxl_cache driver is responsible for validating the CXL.cache system
 * configuration and managing portions of the CXL cache of CXL.cache enabled
 * devices in the system. This driver does not discover devices, a
 * device-specific driver is required for discovery and portions of set up.
 */

/**
 * devm_cxl_add_cachedev - Add a CXL cache device
 * @host: devres alloc/release context and parent for the cachedev
 * @cxlds: CXL device state to associate with the cachedev
 * @ops: optional operations to run in cxl_cache::{probe,remove}() context
 *
 * Upon return the device will have had a chance to attach to the
 * cxl_cache driver. This may fail if the CXL topology is not ready
 * (hardware CXL link down, or software platform CXL root not attached)
 * or the CXL.cache system configuration is invalid.
 *
 * The cache state of @cxlds needs to be populated before calling this function,
 * use cxl_accel_read_cache_info() to do so.
 */
struct cxl_cachedev *devm_cxl_add_cachedev(struct device *host,
					   struct cxl_dev_state *cxlds)
{
	struct cxl_cachedev *cxlcd;
	int rc;

	cxlcd = cxl_cachedev_alloc(cxlds);
	if (IS_ERR(cxlcd))
		return cxlcd;

	rc = dev_set_name(&cxlcd->dev, "cache%d", cxlcd->id);
	if (rc) {
		put_device(&cxlcd->dev);
		return ERR_PTR(rc);
	}

	cxlcd = devm_cxl_cachedev_add_or_reset(host, cxlcd);
	if (IS_ERR(cxlcd))
		return cxlcd;

	guard(device)(&cxlcd->dev);
	if (!cxlcd->dev.driver)
		return ERR_PTR(-ENXIO);

	return cxlcd;
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_add_cachedev, "CXL");

static int cxl_cache_probe(struct device *dev)
{
	struct cxl_cachedev *cxlcd = to_cxl_cachedev(dev);
	struct cxl_dev_state *cxlds = cxlcd->cxlds;
	struct device *endpoint_parent;
	struct cxl_dport *dport;
	int rc;

	rc = devm_cxl_enumerate_ports(dev, cxlds);
	if (rc)
		return rc;

	struct cxl_port *parent_port __free(put_cxl_port) =
		cxl_dev_find_port(dev, &dport);
	if (!parent_port) {
		dev_err(dev, "CXL port topology not found\n");
		return -ENXIO;
	}

	if (dport->rch)
		endpoint_parent = parent_port->uport_dev;
	else
		endpoint_parent = &parent_port->dev;

	scoped_guard(device, endpoint_parent) {
		if (!cxlds->cxlmd)
			cxl_dport_init_ras_reporting(dport, dev);
	}

	return 0;
}

static struct cxl_driver cxl_cache_driver = {
	.name = "cxl_cache",
	.probe = cxl_cache_probe,
	.drv = {
		/*
		 * Needed to guarantee probe and set up order for Type 1/2
		 * vendor drivers.
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
