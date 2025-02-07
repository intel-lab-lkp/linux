/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PCI Endpoint *Function* side MSI header file
 *
 * Copyright (C) 2024 NXP
 * Author: Frank Li <Frank.Li@nxp.com>
 */

#ifndef __PCI_EP_MSI__
#define __PCI_EP_MSI__

#ifdef CONFIG_PCI_ENDPOINT
int pci_epf_msi_domain_get_msi_rid(struct device *dev, u32 *rid);
#else
static inline int pci_epf_msi_domain_get_msi_rid(struct device *dev, u32 *rid)
{
	return -EINVAL;
}
#endif

struct pci_epf;

int pci_epf_alloc_doorbell(struct pci_epf *epf, u16 nums);
void pci_epf_free_doorbell(struct pci_epf *epf);

#endif /* __PCI_EP_MSI__ */
