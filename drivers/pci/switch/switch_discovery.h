/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  PCI Switch Discovery module
 *
 *  Copyright (c) 2024  Broadcom Inc.
 *
 *  Authors: Broadcom Inc.
 *           Sumanesh Samanta <sumanesh.samanta@broadcom.com>
 *           Shivasharan S <shivasharan.srikanteshwara@broadcom.com>
 */

#ifndef PCI_SWITCH_DISC_H
#define PCI_SWITCH_DISC_H

#define SWD_MAX_SWITCH		32
#define SWD_MAX_VER_PER_SWITCH	8

#define SWD_MAX_VIRT_SWITCH	(SWD_MAX_SWITCH * SWD_MAX_VER_PER_SWITCH)
#define SWD_MAX_CHAR		16
#define SWITCH_DISC_VERSION	"0.1.1"
#define SWD_DIR_STRING		"pci_switch_link"
#define SWD_LINK_DIR_STRING	"virtual_switch_links"
#define SWD_SCAN_DONE		"done\n"

#define SWD_FILE_NAME_STRING	refresh_switch_toplogy

/* Broadcom Vendor Specific definitions */
#define PCI_VENDOR_ID_LSI			0x1000
#define PCI_DEVICE_ID_BRCM_PEX_89000_HLC	0xC030
#define PCI_DEVICE_ID_BRCM_PEX_89000_LLC	0xC034

#define P2PMASK(x)		(((x) & 0x300) >> 8)
#define SECURE_PART(x)		((x) & 0x8)
#define INTER_SWITCH_LINK	0x2

struct switch_data {
	int  devfn;
	struct pci_bus *bus;
	char serial_num[SWD_MAX_CHAR];
};

bool sw_disc_check_virtual_link(struct pci_dev *a, struct pci_dev *b);

#endif /* PCI_SWITCH_DISC_H */
