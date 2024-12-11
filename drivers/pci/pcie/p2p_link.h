/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Purpose:	PCI Express P2P link discovery
 *
 * Copyright (C) 2024 Broadcom Inc.
 */

#ifndef _P2P_LINK_H_
#define _P2P_LINK_H_

/* P2P Link supported device IDs */
#define PCI_DEVICE_ID_BRCM_PEX_89000_HLC	0xC030
#define PCI_DEVICE_ID_BRCM_PEX_89000_LLC	0xC034

#define PCIE_BRCM_SW_P2P_VSEC_ID		0x1
#define PCIE_BRCM_SW_P2P_MODE_VSEC_OFFSET	0xC
#define PCIE_BRCM_SW_P2P_MODE_MASK		GENMASK(9, 8)
#define PCIE_BRCM_SW_P2P_MODE_INTER_SW_LINK	0x2
#define PCIE_BRCM_SW_DSN_P2P_STATUS		BIT(3)

#ifdef CONFIG_PCIE_P2P_LINK
void p2p_link_sysfs_update_group(struct pci_dev *pdev);

bool pcie_port_is_p2p_link_available(struct pci_dev *a, struct pci_dev *b);
#else
static inline void p2p_link_sysfs_update_group(struct pci_dev *pdev) { }
static inline bool pcie_port_is_p2p_link_available(struct pci_dev *a, struct pci_dev *b)
{
	return false;
}
#endif
#endif /* _P2P_LINK_H_ */
