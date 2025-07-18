// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2025 AMD Corporation. All rights reserved. */
/* Copyright(c) 2025 Intel Corporation. All rights reserved. */
#include <linux/pci.h>
#include <linux/aer.h>
#include <cxlpci.h>
#include <cxlmem.h>
#include <cxl.h>
#include "core.h"

static void cxl_dport_map_rch_aer(struct cxl_dport *dport)
{
	resource_size_t aer_phys;
	struct device *host;
	u16 aer_cap;

	aer_cap = cxl_rcrb_to_aer(dport->dport_dev, dport->rcrb.base);
	if (aer_cap) {
		host = dport->reg_map.host;
		aer_phys = aer_cap + dport->rcrb.base;
		dport->regs.dport_aer = devm_cxl_iomap_block(host, aer_phys,
						sizeof(struct aer_capability_regs));
	}
}

static void cxl_dport_map_ras(struct cxl_dport *dport)
{
	struct cxl_register_map *map = &dport->reg_map;
	struct device *dev = dport->dport_dev;

	if (!map->component_map.ras.valid)
		dev_dbg(dev, "RAS registers not found\n");
	else if (cxl_map_component_regs(map, &dport->regs.component,
					BIT(CXL_CM_CAP_CAP_ID_RAS)))
		dev_dbg(dev, "Failed to map RAS capability.\n");
}

static void cxl_disable_rch_root_ints(struct cxl_dport *dport)
{
	void __iomem *aer_base = dport->regs.dport_aer;
	u32 aer_cmd_mask, aer_cmd;

	if (!aer_base)
		return;

	/*
	 * Disable RCH root port command interrupts.
	 * CXL 3.0 12.2.1.1 - RCH Downstream Port-detected Errors
	 *
	 * This sequence may not be necessary. CXL spec states disabling
	 * the root cmd register's interrupts is required. But, PCI spec
	 * shows these are disabled by default on reset.
	 */
	aer_cmd_mask = (PCI_ERR_ROOT_CMD_COR_EN |
			PCI_ERR_ROOT_CMD_NONFATAL_EN |
			PCI_ERR_ROOT_CMD_FATAL_EN);
	aer_cmd = readl(aer_base + PCI_ERR_ROOT_COMMAND);
	aer_cmd &= ~aer_cmd_mask;
	writel(aer_cmd, aer_base + PCI_ERR_ROOT_COMMAND);
}

/**
 * cxl_dport_init_ras_reporting - Setup CXL RAS report on this dport
 * @dport: the cxl_dport that needs to be initialized
 * @host: host device for devm operations
 */
void cxl_dport_init_ras_reporting(struct cxl_dport *dport, struct device *host)
{
	dport->reg_map.host = host;
	cxl_dport_map_ras(dport);

	if (dport->rch) {
		struct pci_host_bridge *host_bridge = to_pci_host_bridge(dport->dport_dev);

		if (!host_bridge->native_aer)
			return;

		cxl_dport_map_rch_aer(dport);
		cxl_disable_rch_root_ints(dport);
	}
}
EXPORT_SYMBOL_NS_GPL(cxl_dport_init_ras_reporting, "CXL");

static void cxl_handle_rdport_cor_ras(struct cxl_dev_state *cxlds,
					  struct cxl_dport *dport)
{
	return __cxl_handle_cor_ras(cxlds, dport->regs.ras);
}

static bool cxl_handle_rdport_ras(struct cxl_dev_state *cxlds,
				       struct cxl_dport *dport)
{
	return __cxl_handle_ras(cxlds, dport->regs.ras);
}

/*
 * Copy the AER capability registers using 32 bit read accesses.
 * This is necessary because RCRB AER capability is MMIO mapped. Clear the
 * status after copying.
 *
 * @aer_base: base address of AER capability block in RCRB
 * @aer_regs: destination for copying AER capability
 */
static bool cxl_rch_get_aer_info(void __iomem *aer_base,
				 struct aer_capability_regs *aer_regs)
{
	int read_cnt = sizeof(struct aer_capability_regs) / sizeof(u32);
	u32 *aer_regs_buf = (u32 *)aer_regs;
	int n;

	if (!aer_base)
		return false;

	/* Use readl() to guarantee 32-bit accesses */
	for (n = 0; n < read_cnt; n++)
		aer_regs_buf[n] = readl(aer_base + n * sizeof(u32));

	writel(aer_regs->uncor_status, aer_base + PCI_ERR_UNCOR_STATUS);
	writel(aer_regs->cor_status, aer_base + PCI_ERR_COR_STATUS);

	return true;
}

/* Get AER severity. Return false if there is no error. */
static bool cxl_rch_get_aer_severity(struct aer_capability_regs *aer_regs,
				     int *severity)
{
	if (aer_regs->uncor_status & ~aer_regs->uncor_mask) {
		if (aer_regs->uncor_status & PCI_ERR_ROOT_FATAL_RCV)
			*severity = AER_FATAL;
		else
			*severity = AER_NONFATAL;
		return true;
	}

	if (aer_regs->cor_status & ~aer_regs->cor_mask) {
		*severity = AER_CORRECTABLE;
		return true;
	}

	return false;
}

void cxl_handle_rdport_errors(struct cxl_dev_state *cxlds)
{
	struct pci_dev *pdev = to_pci_dev(cxlds->dev);
	struct aer_capability_regs aer_regs;
	struct cxl_dport *dport;
	int severity;

	struct cxl_port *port __free(put_cxl_port) =
		cxl_pci_find_port(pdev, &dport);
	if (!port)
		return;

	if (!cxl_rch_get_aer_info(dport->regs.dport_aer, &aer_regs))
		return;

	if (!cxl_rch_get_aer_severity(&aer_regs, &severity))
		return;

	pci_print_aer(pdev, severity, &aer_regs);

	if (severity == AER_CORRECTABLE)
		cxl_handle_rdport_cor_ras(cxlds, dport);
	else
		cxl_handle_rdport_ras(cxlds, dport);
}
