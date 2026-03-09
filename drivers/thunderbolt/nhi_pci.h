/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef __TBT_NHI_PCI_H
#define __TBT_NHI_PCI_H

struct tb_nhi_pci {
	struct pci_dev *pdev;
	struct ida msix_ida;
	struct tb_nhi nhi;
};

static inline struct tb_nhi_pci *nhi_to_pci(struct tb_nhi *nhi)
{
	return container_of(nhi, struct tb_nhi_pci, nhi);
}

#endif
