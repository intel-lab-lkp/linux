// SPDX-License-Identifier: GPL-2.0
/*
 * Implements CXL Timeout & Isolation (CXL 3.0 12.3.2) interrupt support as a
 * PCIE port service driver. The driver is set up such that near all of the
 * work for setting up and handling interrupts are in this file, while the
 * CXL core enables the interrupts during port enumeration.
 *
 * Copyright (C) 2024, Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Author: Ben Cheatham <Benjamin.Cheatham@amd.com>
 */

#define pr_fmt(fmt) "cxl_timeout: " fmt
#define dev_fmt pr_fmt

#include <linux/pci.h>
#include <linux/acpi.h>

#include "../../cxl/cxlpci.h"
#include "portdrv.h"

struct cxl_timeout {
	struct pcie_device *dev;
	void __iomem *regs;
	u32 cap;
};

struct pcie_cxlt_data {
	struct cxl_timeout *cxlt;
	struct cxl_dport *dport;
};

static int cxl_map_timeout_regs(struct pci_dev *port,
				struct cxl_register_map *map,
				struct cxl_component_regs *regs)
{
	int rc = 0;

	rc = cxl_find_regblock(port, CXL_REGLOC_RBI_COMPONENT, map);
	if (rc)
		return rc;

	rc = cxl_setup_regs(map);
	if (rc)
		return rc;

	rc = cxl_map_component_regs(map, regs,
				    BIT(CXL_CM_CAP_CAP_ID_TIMEOUT));
	return rc;
}

static void cxl_unmap_timeout_regs(struct pci_dev *port,
				   struct cxl_register_map *map,
				   struct cxl_component_regs *regs)
{
	struct cxl_reg_map *timeout_map = &map->component_map.timeout;

	devm_iounmap(map->host, regs->timeout);
	devm_release_mem_region(map->host, map->resource + timeout_map->offset,
				timeout_map->size);
}

static struct cxl_timeout *cxl_create_cxlt(struct pcie_device *dev)
{
	struct cxl_component_regs *regs;
	struct cxl_register_map *map;
	struct cxl_timeout *cxlt;
	int rc;

	regs = devm_kmalloc(&dev->device, sizeof(*regs), GFP_KERNEL);
	if (!regs)
		return ERR_PTR(-ENOMEM);

	map = devm_kmalloc(&dev->device, sizeof(*map), GFP_KERNEL);
	if (!map) {
		devm_kfree(&dev->device, regs);
		return ERR_PTR(-ENOMEM);
	}

	rc = cxl_map_timeout_regs(dev->port, map, regs);
	if (rc)
		goto err;

	cxlt = devm_kmalloc(&dev->device, sizeof(*cxlt), GFP_KERNEL);
	if (!cxlt)
		goto err;

	cxlt->regs = regs->timeout;
	cxlt->dev = dev;
	cxlt->cap = readl(cxlt->regs + CXL_TIMEOUT_CAPABILITY_OFFSET);

	return cxlt;

err:
	cxl_unmap_timeout_regs(dev->port, map, regs);
	return ERR_PTR(rc);
}

int cxl_find_timeout_cap(struct pci_dev *dev, u32 *cap)
{
	struct cxl_component_regs regs;
	struct cxl_register_map map;
	int rc = 0;

	rc = cxl_map_timeout_regs(dev, &map, &regs);
	if (rc)
		return rc;

	*cap = readl(regs.timeout + CXL_TIMEOUT_CAPABILITY_OFFSET);
	cxl_unmap_timeout_regs(dev, &map, &regs);

	return rc;
}

static struct pcie_cxlt_data *cxlt_create_pdata(struct pcie_device *dev)
{
	struct pcie_cxlt_data *data;

	data = devm_kzalloc(&dev->device, sizeof(*data), GFP_KERNEL);
	if (IS_ERR_OR_NULL(data))
		return ERR_PTR(-ENOMEM);

	data->cxlt = cxl_create_cxlt(dev);
	if (IS_ERR_OR_NULL(data->cxlt))
		return ERR_PTR(PTR_ERR(data->cxlt));

	data->dport = NULL;

	return data;
}

static void cxl_disable_timeout(void *data)
{
	struct cxl_timeout *cxlt = data;
	u32 cntrl = readl(cxlt->regs + CXL_TIMEOUT_CONTROL_OFFSET);

	cntrl &= ~CXL_TIMEOUT_CONTROL_MEM_TIMEOUT_ENABLE;
	writel(cntrl, cxlt->regs + CXL_TIMEOUT_CONTROL_OFFSET);
}

static int cxl_enable_timeout(struct pcie_device *dev, struct cxl_timeout *cxlt)
{
	u32 cntrl;

	if (!cxlt || !FIELD_GET(CXL_TIMEOUT_CAP_MEM_TIMEOUT_SUPP, cxlt->cap))
		return -ENXIO;

	cntrl = readl(cxlt->regs + CXL_TIMEOUT_CONTROL_OFFSET);
	cntrl |= CXL_TIMEOUT_CONTROL_MEM_TIMEOUT_ENABLE;
	writel(cntrl, cxlt->regs + CXL_TIMEOUT_CONTROL_OFFSET);

	return devm_add_action_or_reset(&dev->device, cxl_disable_timeout,
					cxlt);
}

static int cxl_timeout_probe(struct pcie_device *dev)
{
	struct pci_dev *port = dev->port;
	struct pcie_cxlt_data *pdata;
	struct cxl_timeout *cxlt;
	int rc = 0;

	/* Limit to CXL root ports */
	if (!pci_find_dvsec_capability(port, PCI_DVSEC_VENDOR_ID_CXL,
				       CXL_DVSEC_PORT_EXTENSIONS))
		return -ENODEV;

	pdata = cxlt_create_pdata(dev);
	if (IS_ERR_OR_NULL(pdata))
		return PTR_ERR(pdata);

	set_service_data(dev, pdata);
	cxlt = pdata->cxlt;

	rc = cxl_enable_timeout(dev, cxlt);
	if (rc)
		pci_dbg(dev->port, "Failed to enable CXL.mem timeout: %d\n",
			rc);

	return rc;
}

static struct pcie_port_service_driver cxltdriver = {
	.name		= "cxl_timeout",
	.port_type	= PCI_EXP_TYPE_ROOT_PORT,
	.service	= PCIE_PORT_SERVICE_CXLT,

	.probe		= cxl_timeout_probe,
};

int __init pcie_cxlt_init(void)
{
	return pcie_port_service_register(&cxltdriver);
}

MODULE_IMPORT_NS(CXL);
