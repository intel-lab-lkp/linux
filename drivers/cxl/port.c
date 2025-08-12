// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2022 Intel Corporation. All rights reserved. */
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "cxlmem.h"
#include "cxlpci.h"

/**
 * DOC: cxl port
 *
 * The port driver enumerates dport via PCI and scans for HDM
 * (Host-managed-Device-Memory) decoder resources via the
 * @component_reg_phys value passed in by the agent that registered the
 * port. All descendant ports of a CXL root port (described by platform
 * firmware) are managed in this drivers context. Each driver instance
 * is responsible for tearing down the driver context of immediate
 * descendant ports. The locking for this is validated by
 * CONFIG_PROVE_CXL_LOCKING.
 *
 * The primary service this driver provides is presenting APIs to other
 * drivers to utilize the decoders, and indicating to userspace (via bind
 * status) the connectivity of the CXL.mem protocol throughout the
 * PCIe topology.
 */

static void schedule_detach(void *cxlmd)
{
	schedule_cxl_memdev_detach(cxlmd);
}

static int discover_region(struct device *dev, void *unused)
{
	struct cxl_endpoint_decoder *cxled;
	int rc;

	if (!is_endpoint_decoder(dev))
		return 0;

	cxled = to_cxl_endpoint_decoder(dev);
	if ((cxled->cxld.flags & CXL_DECODER_F_ENABLE) == 0)
		return 0;

	if (cxled->state != CXL_DECODER_STATE_AUTO)
		return 0;

	/*
	 * Region enumeration is opportunistic, if this add-event fails,
	 * continue to the next endpoint decoder.
	 */
	rc = cxl_add_to_region(cxled);
	if (rc)
		dev_dbg(dev, "failed to add to region: %#llx-%#llx\n",
			cxled->cxld.hpa_range.start, cxled->cxld.hpa_range.end);

	return 0;
}

static int cxl_dport_init_bi(struct cxl_dport *dport)
{
	struct cxl_register_map *map = &dport->reg_map;
	struct device *dev = dport->dport_dev;

	if (!cxl_pci_flit_256(to_pci_dev(dev)))
		return 0;

	if (!map->component_map.bi.valid) {
		dev_dbg(dev, "BI decoder registers not found\n");
		return 0;
	}

	if (cxl_map_component_regs(map, &dport->regs.component,
				   BIT(CXL_CM_CAP_CAP_ID_BI_DECODER))) {
		dev_dbg(dev, "Failed to map BI decoder capability.\n");
		return -EINVAL;
	}

	return 0;
}

static void cxl_uport_init_bi(struct cxl_port *port, struct device *host)
{
	struct cxl_register_map *map = &port->reg_map;

	if (!map->component_map.bi.valid) {
		dev_dbg(host, "BI RT registers not found\n");
		return;
	}

	map->host = host;
	if (cxl_map_component_regs(map, &port->uport_regs,
				   BIT(CXL_CM_CAP_CAP_ID_BI_RT)))
		dev_dbg(&port->dev, "Failed to map BI RT capability\n");
}

static void cxl_switch_port_init_bi(struct cxl_port *port)
{
	struct cxl_dport *parent_dport = port->parent_dport;

	if (is_cxl_root(to_cxl_port(port->dev.parent)))
		return;

	if (dev_is_pci(&port->dev) && !cxl_pci_flit_256(to_pci_dev(&port->dev)))
		return;

	if (parent_dport && dev_is_pci(parent_dport->dport_dev)) {
		struct pci_dev *pdev = to_pci_dev(parent_dport->dport_dev);

		switch (pci_pcie_type(pdev)) {
		case PCI_EXP_TYPE_ROOT_PORT:
		case PCI_EXP_TYPE_DOWNSTREAM:
			cxl_dport_init_bi(parent_dport);
			break;
		default:
			break;
		}
	}

	cxl_uport_init_bi(port, &port->dev);
}

static int cxl_switch_port_probe(struct cxl_port *port)
{
	struct cxl_hdm *cxlhdm;
	int rc;

	/* Cache the data early to ensure is_visible() works */
	read_cdat_data(port);

	rc = devm_cxl_port_enumerate_dports(port);
	if (rc < 0)
		return rc;

	cxl_switch_parse_cdat(port);

	cxl_switch_port_init_bi(port);

	cxlhdm = devm_cxl_setup_hdm(port, NULL);
	if (!IS_ERR(cxlhdm))
		return devm_cxl_enumerate_decoders(cxlhdm, NULL);

	if (PTR_ERR(cxlhdm) != -ENODEV) {
		dev_err(&port->dev, "Failed to map HDM decoder capability\n");
		return PTR_ERR(cxlhdm);
	}

	if (rc == 1) {
		dev_dbg(&port->dev, "Fallback to passthrough decoder\n");
		return devm_cxl_add_passthrough_decoder(port);
	}

	dev_err(&port->dev, "HDM decoder capability not found\n");
	return -ENXIO;
}

static int cxl_endpoint_port_probe(struct cxl_port *port)
{
	struct cxl_endpoint_dvsec_info info = { .port = port };
	struct cxl_memdev *cxlmd = to_cxl_memdev(port->uport_dev);
	struct cxl_dev_state *cxlds = cxlmd->cxlds;
	struct cxl_hdm *cxlhdm;
	int rc;

	rc = cxl_dvsec_rr_decode(cxlds, &info);
	if (rc < 0)
		return rc;

	cxlhdm = devm_cxl_setup_hdm(port, &info);
	if (IS_ERR(cxlhdm)) {
		if (PTR_ERR(cxlhdm) == -ENODEV)
			dev_err(&port->dev, "HDM decoder registers not found\n");
		return PTR_ERR(cxlhdm);
	}

	/* Cache the data early to ensure is_visible() works */
	read_cdat_data(port);
	cxl_endpoint_parse_cdat(port);

	cxl_dport_init_bi(port->parent_dport);

	get_device(&cxlmd->dev);
	rc = devm_add_action_or_reset(&port->dev, schedule_detach, cxlmd);
	if (rc)
		return rc;

	rc = cxl_hdm_decode_init(cxlds, cxlhdm, &info);
	if (rc)
		return rc;

	rc = devm_cxl_enumerate_decoders(cxlhdm, &info);
	if (rc)
		return rc;

	/*
	 * Now that all endpoint decoders are successfully enumerated, try to
	 * assemble regions from committed decoders
	 */
	device_for_each_child(&port->dev, NULL, discover_region);

	return 0;
}

static int cxl_port_probe(struct device *dev)
{
	struct cxl_port *port = to_cxl_port(dev);

	if (is_cxl_endpoint(port))
		return cxl_endpoint_port_probe(port);
	return cxl_switch_port_probe(port);
}

static ssize_t CDAT_read(struct file *filp, struct kobject *kobj,
			 const struct bin_attribute *bin_attr, char *buf,
			 loff_t offset, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct cxl_port *port = to_cxl_port(dev);

	if (!port->cdat_available)
		return -ENXIO;

	if (!port->cdat.table)
		return 0;

	return memory_read_from_buffer(buf, count, &offset,
				       port->cdat.table,
				       port->cdat.length);
}

static const BIN_ATTR_ADMIN_RO(CDAT, 0);

static umode_t cxl_port_bin_attr_is_visible(struct kobject *kobj,
					    const struct bin_attribute *attr, int i)
{
	struct device *dev = kobj_to_dev(kobj);
	struct cxl_port *port = to_cxl_port(dev);

	if ((attr == &bin_attr_CDAT) && port->cdat_available)
		return attr->attr.mode;

	return 0;
}

static const struct bin_attribute *const cxl_cdat_bin_attributes[] = {
	&bin_attr_CDAT,
	NULL,
};

static const struct attribute_group cxl_cdat_attribute_group = {
	.bin_attrs = cxl_cdat_bin_attributes,
	.is_bin_visible = cxl_port_bin_attr_is_visible,
};

static const struct attribute_group *cxl_port_attribute_groups[] = {
	&cxl_cdat_attribute_group,
	NULL,
};

static struct cxl_driver cxl_port_driver = {
	.name = "cxl_port",
	.probe = cxl_port_probe,
	.id = CXL_DEVICE_PORT,
	.drv = {
		.dev_groups = cxl_port_attribute_groups,
	},
};

static int __init cxl_port_init(void)
{
	return cxl_driver_register(&cxl_port_driver);
}
/*
 * Be ready to immediately enable ports emitted by the platform CXL root
 * (e.g. cxl_acpi) when CONFIG_CXL_PORT=y.
 */
subsys_initcall(cxl_port_init);

static void __exit cxl_port_exit(void)
{
	cxl_driver_unregister(&cxl_port_driver);
}
module_exit(cxl_port_exit);

MODULE_DESCRIPTION("CXL: Port enumeration and services");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS("CXL");
MODULE_ALIAS_CXL(CXL_DEVICE_PORT);
