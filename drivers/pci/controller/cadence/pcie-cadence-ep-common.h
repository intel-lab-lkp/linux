/* SPDX-License-Identifier: GPL-2.0 */
// Copyright (c) 2017 Cadence
// Cadence PCIe Endpoint controller driver
// Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>

#ifndef _PCIE_CADENCE_EP_COMMON_H
#define _PCIE_CADENCE_EP_COMMON_H

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/pci-epf.h>
#include <linux/pci-epc.h>
#include "../../pci.h"

#define CDNS_PCIE_EP_MIN_APERTURE		128	/* 128 bytes */
#define CDNS_PCIE_EP_IRQ_PCI_ADDR_NONE		0x1
#define CDNS_PCIE_EP_IRQ_PCI_ADDR_LEGACY	0x3

u8 cdns_pcie_get_fn_from_vfn(struct cdns_pcie *pcie, u8 fn, u8 vfn);
int cdns_pcie_ep_write_header(struct pci_epc *epc, u8 fn, u8 vfn,
			      struct pci_epf_header *hdr);
int cdns_pcie_ep_set_msi(struct pci_epc *epc, u8 fn, u8 vfn, u8 mmc);
int cdns_pcie_ep_get_msi(struct pci_epc *epc, u8 fn, u8 vfn);
int cdns_pcie_ep_get_msix(struct pci_epc *epc, u8 func_no, u8 vfunc_no);
int cdns_pcie_ep_set_msix(struct pci_epc *epc, u8 fn, u8 vfn,
			  u16 interrupts, enum pci_barno bir,
			  u32 offset);
int cdns_pcie_ep_map_msi_irq(struct pci_epc *epc, u8 fn, u8 vfn,
			     phys_addr_t addr, u8 interrupt_num,
			     u32 entry_size, u32 *msi_data,
			     u32 *msi_addr_offset);
const struct pci_epc_features *cdns_pcie_ep_get_features(struct pci_epc *epc,
							 u8 func_no,
							 u8 vfunc_no);

#endif /* _PCIE_CADENCE_EP_COMMON_H */
