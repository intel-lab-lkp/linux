// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 Advanced Micro Devices, Inc. */

#include <linux/pci.h>
#include <cxl/cxl.h>
#include <linux/iommu.h>

#include "cxlcache.h"

/**
 * DOC: cxl cache
 *
 * The cxl_cache driver is responsible for validating the CXL.cache system
 * configuration and providing a common management interface for the CXL cache
 * of CXL.cache enabled devices. This driver does not discover devices; a
 * device-specific driver is required for discovery and portions of set up.
 */

static bool cxl_flexbus_cache_enabled(struct device *host)
{

	u16 dvsec, cap, status;
	struct pci_dev *pdev;
	int rc;

	if (!dev_is_pci(host))
		return false;
	pdev = to_pci_dev(host);

	dvsec = pci_find_dvsec_capability(pdev, PCI_VENDOR_ID_CXL,
					  PCI_DVSEC_CXL_FLEXBUS_PORT);
	if (!dvsec)
		return false;

	rc = pci_read_config_word(pdev,
				  dvsec + PCI_DVSEC_CXL_FLEXBUS_PORT_CAPABILITY,
				  &cap);
	if (rc)
		return false;

	if (!FIELD_GET(PCI_DVSEC_CXL_FLEXBUS_PORT_CAP_CACHE, cap))
		return false;

	rc = pci_read_config_word(pdev,
				  dvsec + PCI_DVSEC_CXL_FLEXBUS_PORT_STATUS,
				  &status);
	if (rc)
		return false;

	return FIELD_GET(PCI_DVSEC_CXL_FLEXBUS_PORT_STATUS_CACHE, status);
}

static int cxl_endpoint_cache_enabled(struct cxl_port *endpoint)
{
	struct cxl_dport *dport_iter = endpoint->parent_dport;
	struct cxl_port *port_iter = dport_iter->port;

	while (!is_cxl_root(port_iter)) {
		/* 
		 * CXL host bridge isn't a PCI device, but we still need to
		 * check the PCIe root port
		 */
		if (dev_is_pci(port_iter->uport_dev) &&
		    !cxl_flexbus_cache_enabled(port_iter->uport_dev)) {
			dev_dbg(port_iter->uport_dev,
				"CXL.cache not supported or enabled\n");
			return -ENXIO;
		}

		/*
		 * Cache can be enabled for dports, but it requires a link
		 * reset. Could be done here, but should probably be done
		 * by the endpoint's driver.
		 */
		if (!cxl_flexbus_cache_enabled(dport_iter->dport_dev)) {
			dev_dbg(dport_iter->dport_dev,
				"CXL.cache not supported or enabled\n");
			return -ENXIO;
		}

		dport_iter = port_iter->parent_dport;
		port_iter = dport_iter->port;
	}

	return 0;
}

static struct cxl_port *find_host_bridge(struct cxl_port *endpoint)
{
	struct cxl_port *parent = endpoint->parent_dport->port;
	struct cxl_port *hb = endpoint;

	if (is_cxl_root(parent))
		return NULL;

	while (!is_cxl_root(parent)) {
		hb = parent;
		parent = hb->parent_dport->port;
	}

	return hb;
}

static int get_num_cachedevs_present(struct cxl_port *host_bridge, u32 *num)
{
	u32 num_cachedevs = 0;
	unsigned long index;
	struct cxl_ep *ep;

	xa_for_each(&host_bridge->endpoints, index, ep) {
		if (is_cxl_cachedev(ep->ep))
			num_cachedevs++;
	}

	*num = num_cachedevs;
	return 0;
}

static void deprogram_cache_id(void *_cxlcd)
{
	struct cxl_cachedev *cxlcd = _cxlcd;
	struct cxl_port *hb = find_host_bridge(cxlcd->endpoint);

	if (cxlcd->cache_id == CXL_CACHE_ID_NO_ID)
		return;

	guard(device)(&hb->dev);
	cxl_cachedev_deprogram_cache_id(cxlcd);
	cxl_free_cache_id(cxlcd);
}

static int program_cache_id(struct cxl_cachedev *cxlcd)
{
	struct cxl_port *endpoint = cxlcd->endpoint;
	struct cxl_port *hb = find_host_bridge(endpoint);
	struct device *dev = &cxlcd->dev;
	u32 num_cachedevs;
	int rc;

	if (!hb)
		return -ENODEV;

	/*
	 * Cache ids are unique to the host bridge, so synchronize programming
	 * around the bridge's device lock
	 */

	guard(device)(&hb->dev);
	rc = get_num_cachedevs_present(hb, &num_cachedevs);
	if (rc)
		return rc;

	if (!cxl_cache_id_supported(cxlcd))
		return num_cachedevs > 1 ? -ENXIO : 0;

	rc = cxl_cachedev_validate_cache_id(cxlcd);
	if (!rc)
		return cxl_allocate_cache_id(cxlcd);

	rc = cxl_allocate_cache_id(cxlcd);
	if (rc)
		return rc;

	rc = cxl_cachedev_program_cache_id(cxlcd);
	if (rc) {
		dev_err(dev, "Failed to program cache id: %d\n", rc);
		cxl_free_cache_id(cxlcd);
	}

	return rc;
}

/**
 * devm_cxl_add_cachedev - Add a CXL cache device
 * @cxlds: CXL device state to associate with the cachedev
 */
struct cxl_cachedev *devm_cxl_add_cachedev(struct cxl_dev_state *cxlds)
{
	return __devm_cxl_add_cachedev(cxlds, NULL);
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_add_cachedev, "CXL");

static int cxl_cachedev_find_snoop_gid(struct cxl_cachedev *cxlcd)
{
	struct cxl_dport *iter;

	for (iter = cxlcd->endpoint->parent_dport;
	     iter && !is_cxl_root(iter->port);
	     iter = iter->port->parent_dport) {
		if (iter->snoop != CXL_SNOOP_FILTER_NO_GROUP_ID)
			return iter->snoop;
	}

	return -ENXIO;
}

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

	rc = cxl_endpoint_cache_enabled(cxlcd->endpoint);
	if (rc) {
		dev_err(dev, "CXL.cache not enabled on parent port(s)");
		return rc;
	}

	rc = program_cache_id(cxlcd);
	if (rc)
		return rc;

	rc = devm_add_action_or_reset(dev, deprogram_cache_id, cxlcd);
 	if (rc)
 		return rc;

	rc = cxl_cachedev_find_snoop_gid(cxlcd);
	if (rc < 0)
		return rc;

	cxlcd->cxlds->cstate.gid = rc;

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
