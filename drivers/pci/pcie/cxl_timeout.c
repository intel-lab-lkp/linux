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

#define NUM_CXL_TIMEOUT_RANGES 9

static u32 num_cxlt_devs;

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

int pcie_cxl_find_timeout_cap(struct pci_dev *dev, u32 *cap)
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

bool pcie_supports_cxl_timeout_interrupts(u32 cap)
{
	if (!(cap & CXL_TIMEOUT_CAP_INTR_SUPP))
		return false;

	return (cap & CXL_TIMEOUT_CAP_MEM_ISO_SUPP) ||
		(cap & CXL_TIMEOUT_CAP_MEM_TIMEOUT_SUPP);
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

int pcie_cxlt_register_dport(struct cxl_dport *dport)
{
	struct device *dev = dport->dport_dev;
	struct pcie_device *pcie_dev;
	struct pcie_cxlt_data *pdata;
	struct pci_dev *pdev;

	if (!dev_is_pci(dev))
		return -ENXIO;

	pdev = to_pci_dev(dev);

	dev = pcie_port_find_device(pdev, PCIE_PORT_SERVICE_CXLT);
	if (!dev) {
		dev_warn(dev,
			 "Device is not registered with cxl_timeout driver.\n");
		return -ENODEV;
	}

	pcie_dev = to_pcie_device(dev);

	pdata = get_service_data(pcie_dev);
	pdata->dport = dport;

	return 0;
}
EXPORT_SYMBOL_GPL(pcie_cxlt_register_dport);

void pcie_cxlt_unregister_dport(struct cxl_dport *dport)
{
	struct device *dev = dport->dport_dev;
	struct pcie_device *pcie_dev;
	struct pcie_cxlt_data *pdata;
	struct pci_dev *pdev;

	if (!dev_is_pci(dev))
		return;

	pdev = to_pci_dev(dev);

	dev = pcie_port_find_device(pdev, PCIE_PORT_SERVICE_CXLT);
	if (!dev) {
		dev_dbg(dev,
			"Device was not registered with cxl_timeout driver.\n");
		return;
	}

	pcie_dev = to_pcie_device(dev);
	pdata = get_service_data(pcie_dev);
	pdata->dport = NULL;
}
EXPORT_SYMBOL_GPL(pcie_cxlt_unregister_dport);

struct cxl_timeout_wq_data {
	struct work_struct w;
	struct cxl_dport *dport;
};

static struct workqueue_struct *cxl_timeout_wq;

static void cxl_timeout_handler(struct work_struct *w)
{
	struct cxl_timeout_wq_data *data =
		container_of(w, struct cxl_timeout_wq_data, w);
	struct cxl_dport *dport = data->dport;
	struct cxl_port *port;
	struct cxl_region_ref *ref;
	unsigned long index;
	bool kill_regions;

	if (!dport || !dport->port)
		return;

	port = dport->port;

	xa_for_each(&port->regions, index, ref)
		if (cxl_dport_is_in_region(dport, ref))
			kill_regions = true;

	if (kill_regions)
		cxl_port_kill_regions(port);

	kfree(data);
}

irqreturn_t cxl_timeout_thread(int irq, void *data)
{
	struct cxl_timeout_wq_data *wq_data;
	struct cxl_timeout *cxlt = data;
	struct pcie_device *pcie_dev = cxlt->dev;
	struct pcie_cxlt_data *pdata;
	struct cxl_dport *dport;
	u32 status;

	/*
	 * If the CXL core didn't register a cxl_dport with this PCIe device,
	 * then dport enumeration failed and there's nothing to do CXL-wise.
	 */
	pdata = get_service_data(pcie_dev);
	if (!pdata || !pdata->dport)
		return IRQ_HANDLED;

	dport = pdata->dport;

	status = readl(cxlt->regs + CXL_TIMEOUT_STATUS_OFFSET);
	if (!(status & CXL_TIMEOUT_STATUS_MEM_ISO
	      || status & CXL_TIMEOUT_STATUS_MEM_TIMEOUT))
		return IRQ_HANDLED;

	dport->isolated = true;

	wq_data = kzalloc(sizeof(struct cxl_timeout_wq_data), GFP_NOWAIT);
	if (!wq_data)
		return IRQ_NONE;

	wq_data->dport = dport;

	INIT_WORK(&wq_data->w, cxl_timeout_handler);
	queue_work(cxl_timeout_wq, &wq_data->w);

	return IRQ_HANDLED;
}

static int cxl_enable_interrupts(struct pcie_device *dev,
				 struct cxl_timeout *cxlt)
{
	if (!cxlt || !FIELD_GET(CXL_TIMEOUT_CAP_INTR_SUPP, cxlt->cap))
		return -ENXIO;

	return devm_request_threaded_irq(&dev->device, dev->irq, NULL,
					 cxl_timeout_thread,
					 IRQF_SHARED | IRQF_ONESHOT, "cxltdrv",
					 cxlt);
}

static bool cxl_validate_timeout_range(struct cxl_timeout *cxlt, u8 range)
{
	u8 timeout_ranges = FIELD_GET(CXL_TIMEOUT_CAP_MEM_TIMEOUT_MASK,
				      cxlt->cap);

	if (!timeout_ranges)
		return false;

	switch (range) {
	case CXL_TIMEOUT_TIMEOUT_RANGE_DEFAULT:
		return true;
	case CXL_TIMEOUT_TIMEOUT_RANGE_A1:
	case CXL_TIMEOUT_TIMEOUT_RANGE_A2:
		return timeout_ranges & BIT(0);
	case CXL_TIMEOUT_TIMEOUT_RANGE_B1:
	case CXL_TIMEOUT_TIMEOUT_RANGE_B2:
		return timeout_ranges & BIT(1);
	case CXL_TIMEOUT_TIMEOUT_RANGE_C1:
	case CXL_TIMEOUT_TIMEOUT_RANGE_C2:
		return timeout_ranges & BIT(2);
	case CXL_TIMEOUT_TIMEOUT_RANGE_D1:
	case CXL_TIMEOUT_TIMEOUT_RANGE_D2:
		return timeout_ranges & BIT(3);
	default:
		pci_info(cxlt->dev->port, "Invalid timeout range: %d\n",
			 range);
		return false;
	}
}

static int cxl_set_mem_timeout_range(struct cxl_timeout *cxlt, u8 range)
{
	u32 cntrl;

	if (!cxlt)
		return -ENXIO;

	if (!FIELD_GET(CXL_TIMEOUT_CAP_MEM_TIMEOUT_MASK, cxlt->cap)
	    || !cxl_validate_timeout_range(cxlt, range))
		return -ENXIO;

	cntrl = readl(cxlt->regs + CXL_TIMEOUT_CONTROL_OFFSET);
	cntrl &= ~CXL_TIMEOUT_CONTROL_MEM_TIMEOUT_MASK;
	cntrl |= CXL_TIMEOUT_CONTROL_MEM_TIMEOUT_MASK & range;
	writel(cntrl, cxlt->regs + CXL_TIMEOUT_CONTROL_OFFSET);

	pci_dbg(cxlt->dev->port,
		 "Timeout & isolation timeout set to range 0x%x\n", range);
	return 0;
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

static void cxl_disable_isolation(void *data)
{
	struct cxl_timeout *cxlt = data;
	u32 cntrl = readl(cxlt->regs + CXL_TIMEOUT_CONTROL_OFFSET);

	cntrl &= ~CXL_TIMEOUT_CONTROL_MEM_ISO_ENABLE;
	writel(cntrl, cxlt->regs + CXL_TIMEOUT_CONTROL_OFFSET);
}

static int cxl_enable_isolation(struct pcie_device *dev,
				struct cxl_timeout *cxlt)
{
	u32 cntrl;

	if (!cxlt || !FIELD_GET(CXL_TIMEOUT_CAP_MEM_ISO_SUPP, cxlt->cap))
		return -ENXIO;

	cntrl = readl(cxlt->regs + CXL_TIMEOUT_CONTROL_OFFSET);
	cntrl |= CXL_TIMEOUT_CONTROL_MEM_ISO_ENABLE;
	writel(cntrl, cxlt->regs + CXL_TIMEOUT_CONTROL_OFFSET);

	return devm_add_action_or_reset(&dev->device, cxl_disable_isolation,
					cxlt);
}

static ssize_t timeout_range_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct pcie_device *pdev = to_pcie_device(dev);
	struct pcie_cxlt_data *pdata = get_service_data(pdev);
	u32 cntrl, range;

	if (!pdata || !pdata->cxlt)
		return -ENXIO;

	cntrl = readl(pdata->cxlt->regs + CXL_TIMEOUT_CONTROL_OFFSET);

	range = FIELD_GET(CXL_TIMEOUT_CONTROL_MEM_TIMEOUT_MASK, cntrl);
	return sysfs_emit(buf, "%u\n", range);
}

static ssize_t timeout_range_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct pcie_device *pdev = to_pcie_device(dev);
	struct pcie_cxlt_data *pdata = get_service_data(pdev);
	u8 range;
	int rc;

	if (kstrtou8(buf, 16, &range) < 0)
		return -EINVAL;

	if (!pdata || !pdata->cxlt)
		return -ENXIO;

	rc = cxl_set_mem_timeout_range(pdata->cxlt, range);
	if (rc)
		return rc;

	return count;
}
static DEVICE_ATTR_RW(timeout_range);

const struct cxl_timeout_range {
	const char *str;
	u8 range_val;
} cxl_timeout_ranges[NUM_CXL_TIMEOUT_RANGES] = {
		{ "Default range (50us-10ms)",
			CXL_TIMEOUT_TIMEOUT_RANGE_DEFAULT },
		{ "Range A (50us-100us)",
			CXL_TIMEOUT_TIMEOUT_RANGE_A1 },
		{ "Range A (1ms-10ms)",
			CXL_TIMEOUT_TIMEOUT_RANGE_A2 },
		{ "Range B (16ms-55ms)",
			CXL_TIMEOUT_TIMEOUT_RANGE_B1 },
		{ "Range B (65ms-210ms)",
			CXL_TIMEOUT_TIMEOUT_RANGE_B2 },
		{ "Range C (260ms-900ms)",
			CXL_TIMEOUT_TIMEOUT_RANGE_C1 },
		{ "Range C (1s-3.5s)",
			CXL_TIMEOUT_TIMEOUT_RANGE_C2 },
		{ "Range D (4s-13s)",
			CXL_TIMEOUT_TIMEOUT_RANGE_D1 },
		{ "Range D (17s-64s)",
			CXL_TIMEOUT_TIMEOUT_RANGE_D2 },
};

static ssize_t available_timeout_ranges_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct pcie_device *pdev = to_pcie_device(dev);
	struct pcie_cxlt_data *pdata = get_service_data(pdev);
	ssize_t count = 0;
	u8 range;

	if (!pdata || !pdata->cxlt)
		return -ENXIO;

	for (int i = 0; i < ARRAY_SIZE(cxl_timeout_ranges); i++) {
		range = cxl_timeout_ranges[i].range_val;

		if (cxl_validate_timeout_range(pdata->cxlt, range)) {
			count += sysfs_emit_at(buf, count, "0x%x\t%s\n",
					       cxl_timeout_ranges[i].range_val,
					       cxl_timeout_ranges[i].str);
		}
	}

	return count;
}
static DEVICE_ATTR_RO(available_timeout_ranges);

static umode_t cxl_timeout_is_visible(struct kobject *kobj,
				       struct attribute *attr, int val)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pcie_device *pdev = to_pcie_device(dev);
	struct pcie_cxlt_data *pdata = get_service_data(pdev);
	u32 cap;

	if (!pdata || !pdata->cxlt)
		return 0;

	cap = pdata->cxlt->cap;

	if ((attr == &dev_attr_timeout_range.attr) &&
	    cap & CXL_TIMEOUT_CAP_MEM_TIMEOUT_SUPP)
		return attr->mode;

	if ((attr == &dev_attr_available_timeout_ranges.attr) &&
	    (FIELD_GET(CXL_TIMEOUT_CAP_MEM_TIMEOUT_MASK, cap)))
		return attr->mode;

	return 0;
}
static struct attribute *cxl_timeout_timeout_attributes[] = {
	&dev_attr_timeout_range.attr,
	&dev_attr_available_timeout_ranges.attr,
	NULL,
};

static struct attribute_group cxl_timeout_timeout_group = {
	.attrs = cxl_timeout_timeout_attributes,
	.is_visible = cxl_timeout_is_visible,
};

static const struct attribute_group *cxl_timeout_attribute_groups[] = {
	&cxl_timeout_timeout_group,
	NULL,
};

static int cxl_timeout_probe(struct pcie_device *dev)
{
	struct pci_dev *port = dev->port;
	struct pcie_cxlt_data *pdata;
	struct cxl_timeout *cxlt;
	bool timeout_enabled;
	int rc;

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

	timeout_enabled = !rc;

	rc = cxl_enable_isolation(dev, cxlt);
	if (rc)
		pci_dbg(dev->port, "Failed to enable CXL.mem isolation: %d\n",
			rc);

	if (rc && !timeout_enabled) {
		pci_info(dev->port,
			 "Failed to enable CXL.mem timeout and isolation.\n");
		return rc;
	}

	rc = cxl_enable_interrupts(dev, cxlt);
	if (rc) {
		pci_info(dev->port,
			"Failed to enable CXL.mem timeout & isolation interrupts: %d\n",
			rc);
	} else {
		pci_info(port, "enabled with IRQ %d\n", dev->irq);
	}

	num_cxlt_devs++;
	return 0;
}

static void cxl_timeout_remove(struct pcie_device *dev)
{
	num_cxlt_devs--;

	if (!num_cxlt_devs)
		destroy_workqueue(cxl_timeout_wq);
}

static struct pcie_port_service_driver cxltdriver = {
	.name		= "cxl_timeout",
	.port_type	= PCI_EXP_TYPE_ROOT_PORT,
	.service	= PCIE_PORT_SERVICE_CXLT,

	.probe		= cxl_timeout_probe,
	.remove		= cxl_timeout_remove,
	.driver		= {
		.dev_groups = cxl_timeout_attribute_groups,
	},
};

int __init pcie_cxlt_init(void)
{
	cxl_timeout_wq = alloc_ordered_workqueue("cxl_timeout", 0);
	if (!cxl_timeout_wq)
		return -ENOMEM;

	num_cxlt_devs = 0;

	return pcie_port_service_register(&cxltdriver);
}

MODULE_IMPORT_NS(CXL);
