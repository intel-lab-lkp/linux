// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Endpoint *Controller* (EPC) MSI library
 *
 * Copyright (C) 2025 NXP
 * Author: Frank Li <Frank.Li@nxp.com>
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of_irq.h>
#include <linux/pci-epc.h>
#include <linux/pci-epf.h>
#include <linux/pci-ep-cfs.h>
#include <linux/pci-ep-msi.h>
#include <linux/slab.h>

static void pci_epf_write_msi_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	struct pci_epf *epf = to_pci_epf(desc->dev);

	if (epf && epf->db_msg && desc->msi_index < epf->num_db)
		memcpy(&epf->db_msg[desc->msi_index].msg, msg, sizeof(*msg));
}

int pci_epf_alloc_doorbell(struct pci_epf *epf, u16 num_db)
{
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	struct irq_domain *dom;
	void *msg;
	u32 rid;
	int ret;
	int i;

	rid = PCI_EPF_DEVID(epf->func_no, epf->vfunc_no);
	dom = of_msi_map_get_device_domain(epc->dev.parent, rid, DOMAIN_BUS_PLATFORM_MSI);
	if (!dom) {
		dev_err(dev, "Can't find msi domain\n");
		return -EINVAL;
	}

	dev_set_msi_domain(dev, dom);

	msg = kcalloc(num_db, sizeof(struct pci_epf_doorbell_msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	epf->num_db = num_db;
	epf->db_msg = msg;

	ret = platform_device_msi_init_and_alloc_irqs(&epf->dev, num_db, pci_epf_write_msi_msg);
	if (ret) {
		/*
		 * The pcie_ep DT node has to specify 'msi-parent' for EP
		 * doorbell support to work. Right now only GIC ITS is
		 * supported. If you have GIC ITS and reached this print,
		 * perhaps you are missing 'msi-map' in DT.
		 */
		dev_err(dev, "Failed to allocate MSI\n");
		kfree(msg);
		return -ENOMEM;
	}

	for (i = 0; i < num_db; i++)
		epf->db_msg[i].virq = msi_get_virq(dev, i);

	return ret;
}
EXPORT_SYMBOL_GPL(pci_epf_alloc_doorbell);

void pci_epf_free_doorbell(struct pci_epf *epf)
{
	platform_device_msi_free_irqs_all(&epf->dev);

	kfree(epf->db_msg);
	epf->db_msg = NULL;
	epf->num_db = 0;
}
EXPORT_SYMBOL_GPL(pci_epf_free_doorbell);
