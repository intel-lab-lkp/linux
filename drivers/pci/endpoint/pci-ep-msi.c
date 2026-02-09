// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Endpoint *Controller* (EPC) MSI library
 *
 * Copyright (C) 2025 NXP
 * Author: Frank Li <Frank.Li@nxp.com>
 */

#include <linux/device.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
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
	struct pci_epc *epc;
	struct pci_epf *epf;

	epc = pci_epc_get(dev_name(msi_desc_to_dev(desc)));
	if (IS_ERR(epc))
		return;

	epf = list_first_entry_or_null(&epc->pci_epf, struct pci_epf, list);

	if (epf && epf->db_msg && desc->msi_index < epf->num_db)
		memcpy(&epf->db_msg[desc->msi_index].msg, msg, sizeof(*msg));

	pci_epc_put(epc);
}

static int pci_epf_alloc_doorbell_embedded(struct pci_epf *epf, u16 num_db)
{
	struct pci_epc *epc = epf->epc;
	const struct pci_epc_aux_resource *dma_ctrl = NULL;
	struct pci_epf_doorbell_msg *msg;
	int count, ret, i, db;

	count = pci_epc_get_aux_resources(epc, epf->func_no, epf->vfunc_no,
					  NULL, 0);
	if (count == -EOPNOTSUPP || count == 0)
		return -ENODEV;
	if (count < 0)
		return count;

	struct pci_epc_aux_resource *res __free(kfree) =
				kcalloc(count, sizeof(*res), GFP_KERNEL);
	if (!res)
		return -ENOMEM;

	ret = pci_epc_get_aux_resources(epc, epf->func_no, epf->vfunc_no,
					res, count);
	if (ret == -EOPNOTSUPP || ret == 0)
		return -ENODEV;
	if (ret < 0)
		return ret;

	count = ret;

	for (i = 0; i < count; i++) {
		if (res[i].type == PCI_EPC_AUX_DMA_CTRL_MMIO) {
			dma_ctrl = &res[i];
			break;
		}
	}
	if (!dma_ctrl)
		return -ENODEV;

	msg = kcalloc(num_db, sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	for (i = 0, db = 0; i < count && db < num_db; i++) {
		u64 addr;

		if (res[i].type != PCI_EPC_AUX_DMA_CHAN_DESC)
			continue;

		if (res[i].u.dma_chan_desc.db_offset >= dma_ctrl->size)
			continue;

		addr = (u64)dma_ctrl->phys_addr + res[i].u.dma_chan_desc.db_offset;

		msg[db].msg.address_lo = (u32)addr;
		msg[db].msg.address_hi = (u32)(addr >> 32);
		msg[db].msg.data = 0;
		msg[db].virq = res[i].u.dma_chan_desc.irq;
		msg[db].irq_flags = IRQF_SHARED;
		msg[db].type = PCI_EPF_DOORBELL_EMBEDDED;
		db++;
	}

	if (db != num_db) {
		kfree(msg);
		return -ENOSPC;
	}

	epf->num_db = num_db;
	epf->db_msg = msg;
	return 0;
}

static int pci_epf_alloc_doorbell_msi(struct pci_epf *epf, u16 num_db)
{
	struct pci_epf_doorbell_msg *msg;
	struct device *dev = &epf->dev;
	struct pci_epc *epc = epf->epc;
	struct irq_domain *domain;
	int ret, i;

	domain = of_msi_map_get_device_domain(epc->dev.parent, 0,
					      DOMAIN_BUS_PLATFORM_MSI);
	if (!domain) {
		dev_err(dev, "Can't find MSI domain for EPC\n");
		return -ENODEV;
	}

	if (!irq_domain_is_msi_parent(domain))
		return -ENODEV;

	if (!irq_domain_is_msi_immutable(domain)) {
		dev_err(dev, "Mutable MSI controller not supported\n");
		return -ENODEV;
	}

	dev_set_msi_domain(epc->dev.parent, domain);

	msg = kcalloc(num_db, sizeof(struct pci_epf_doorbell_msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	for (i = 0; i < num_db; i++) {
		msg[i].irq_flags = 0;
		msg[i].type = PCI_EPF_DOORBELL_MSI;
	}

	epf->num_db = num_db;
	epf->db_msg = msg;

	ret = platform_device_msi_init_and_alloc_irqs(epc->dev.parent, num_db,
						      pci_epf_write_msi_msg);
	if (ret) {
		dev_err(dev, "Failed to allocate MSI\n");
		kfree(msg);
		epf->db_msg = NULL;
		epf->num_db = 0;
		return ret;
	}

	for (i = 0; i < num_db; i++)
		epf->db_msg[i].virq = msi_get_virq(epc->dev.parent, i);

	return 0;
}

int pci_epf_alloc_doorbell(struct pci_epf *epf, u16 num_db)
{
	struct pci_epc *epc = epf->epc;
	struct device *dev = &epf->dev;
	int ret;

	/* TODO: Multi-EPF support */
	if (list_first_entry_or_null(&epc->pci_epf, struct pci_epf, list) != epf) {
		dev_err(dev, "Doorbell doesn't support multiple EPF\n");
		return -EINVAL;
	}

	if (epf->db_msg)
		return -EBUSY;

	ret = pci_epf_alloc_doorbell_msi(epf, num_db);
	if (!ret)
		return 0;

	if (ret != -ENODEV)
		return ret;

	ret = pci_epf_alloc_doorbell_embedded(epf, num_db);
	if (!ret) {
		dev_info(dev, "Using embedded (DMA) doorbell fallback\n");
		return 0;
	}

	dev_err(dev, "Failed to allocate doorbell: %d\n", ret);
	return ret;
}
EXPORT_SYMBOL_GPL(pci_epf_alloc_doorbell);

void pci_epf_free_doorbell(struct pci_epf *epf)
{
	if (!epf->db_msg)
		return;

	if (epf->db_msg[0].type == PCI_EPF_DOORBELL_MSI)
		platform_device_msi_free_irqs_all(epf->epc->dev.parent);

	kfree(epf->db_msg);
	epf->db_msg = NULL;
	epf->num_db = 0;
}
EXPORT_SYMBOL_GPL(pci_epf_free_doorbell);
