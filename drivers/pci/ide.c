// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2024 Intel Corporation. All rights reserved. */

/* PCIe 6.2 section 6.33 Integrity & Data Encryption (IDE) */

#define dev_fmt(fmt) "PCI/IDE: " fmt
#include <linux/pci.h>
#include "pci.h"

static int sel_ide_offset(u16 cap, int stream_id, int nr_ide_mem)
{
	return cap + stream_id * PCI_IDE_SELECTIVE_BLOCK_SIZE(nr_ide_mem);
}

void pci_ide_init(struct pci_dev *pdev)
{
	u16 ide_cap, sel_ide_cap;
	int nr_ide_mem = 0;
	u32 val = 0;

	if (!pci_is_pcie(pdev))
		return;

	ide_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_IDE);
	if (!ide_cap)
		return;

	/*
	 * Check for selective stream capability from endpoint to root-port, and
	 * require consistent number of address association blocks
	 */
	pci_read_config_dword(pdev, ide_cap + PCI_IDE_CAP, &val);
	if ((val & PCI_IDE_CAP_SELECTIVE) == 0)
		return;

	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ENDPOINT) {
		struct pci_dev *rp = pcie_find_root_port(pdev);

		if (!rp->ide_cap)
			return;
	}

	if (val & PCI_IDE_CAP_LINK)
		sel_ide_cap = ide_cap + PCI_IDE_LINK_STREAM +
			      (PCI_IDE_CAP_LINK_TC_NUM(val) + 1) *
				      PCI_IDE_LINK_BLOCK_SIZE;
	else
		sel_ide_cap = ide_cap + PCI_IDE_LINK_STREAM;

	for (int i = 0; i < PCI_IDE_CAP_SELECTIVE_STREAMS_NUM(val); i++) {
		if (i == 0) {
			pci_read_config_dword(pdev, sel_ide_cap, &val);
			nr_ide_mem = PCI_IDE_SEL_CAP_ASSOC_NUM(val);
		} else {
			int offset = sel_ide_offset(sel_ide_cap, i, nr_ide_mem);

			pci_read_config_dword(pdev, offset, &val);

			/*
			 * lets not entertain devices that do not have a
			 * constant number of address association blocks
			 */
			if (PCI_IDE_SEL_CAP_ASSOC_NUM(val) != nr_ide_mem) {
				pci_info(pdev, "Unsupported Selective Stream %d capability\n", i);
				return;
			}
		}
	}

	pdev->ide_cap = ide_cap;
	pdev->sel_ide_cap = sel_ide_cap;
	pdev->nr_ide_mem = nr_ide_mem;
}
