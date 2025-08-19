// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017 Cadence
// Cadence PCIe endpoint controller driver common
// Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>

#include "pcie-cadence.h"
#include "pcie-cadence-ep-common.h"

u8 cdns_pcie_get_fn_from_vfn(struct cdns_pcie *pcie, u8 fn, u8 vfn)
{
	u32 cap = CDNS_PCIE_EP_FUNC_SRIOV_CAP_OFFSET;
	u32 first_vf_offset, stride;

	if (vfn == 0)
		return fn;

	first_vf_offset = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_SRIOV_VF_OFFSET);
	stride = cdns_pcie_ep_fn_readw(pcie, fn, cap +  PCI_SRIOV_VF_STRIDE);
	fn = fn + first_vf_offset + ((vfn - 1) * stride);

	return fn;
}
EXPORT_SYMBOL_GPL(cdns_pcie_get_fn_from_vfn);

int cdns_pcie_ep_write_header(struct pci_epc *epc, u8 fn, u8 vfn,
			      struct pci_epf_header *hdr)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	u32 cap = CDNS_PCIE_EP_FUNC_SRIOV_CAP_OFFSET;
	struct cdns_pcie *pcie = &ep->pcie;
	u32 reg;

	if (vfn > 1) {
		dev_err(&epc->dev, "Only Virtual Function #1 has deviceID\n");
		return -EINVAL;
	} else if (vfn == 1) {
		reg = cap + PCI_SRIOV_VF_DID;
		cdns_pcie_ep_fn_writew(pcie, fn, reg, hdr->deviceid);
		return 0;
	}

	cdns_pcie_ep_fn_writew(pcie, fn, PCI_DEVICE_ID, hdr->deviceid);
	cdns_pcie_ep_fn_writeb(pcie, fn, PCI_REVISION_ID, hdr->revid);
	cdns_pcie_ep_fn_writeb(pcie, fn, PCI_CLASS_PROG, hdr->progif_code);
	cdns_pcie_ep_fn_writew(pcie, fn, PCI_CLASS_DEVICE,
			       hdr->subclass_code | hdr->baseclass_code << 8);
	cdns_pcie_ep_fn_writeb(pcie, fn, PCI_CACHE_LINE_SIZE,
			       hdr->cache_line_size);
	cdns_pcie_ep_fn_writew(pcie, fn, PCI_SUBSYSTEM_ID, hdr->subsys_id);
	cdns_pcie_ep_fn_writeb(pcie, fn, PCI_INTERRUPT_PIN, hdr->interrupt_pin);

	/*
	 * Vendor ID can only be modified from function 0, all other functions
	 * use the same vendor ID as function 0.
	 */
	if (fn == 0) {
		/* Update the vendor IDs. */
		u32 id = CDNS_PCIE_LM_ID_VENDOR(hdr->vendorid) |
			 CDNS_PCIE_LM_ID_SUBSYS(hdr->subsys_vendor_id);

		cdns_pcie_writel(pcie, CDNS_PCIE_LM_ID, id);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(cdns_pcie_ep_write_header);

int cdns_pcie_ep_set_msi(struct pci_epc *epc, u8 fn, u8 vfn, u8 mmc)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	u32 cap = CDNS_PCIE_EP_FUNC_MSI_CAP_OFFSET;
	u16 flags;

	fn = cdns_pcie_get_fn_from_vfn(pcie, fn, vfn);

	/*
	 * Set the Multiple Message Capable bitfield into the Message Control
	 * register.
	 */
	flags = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_FLAGS);
	flags = (flags & ~PCI_MSI_FLAGS_QMASK) | (mmc << 1);
	flags |= PCI_MSI_FLAGS_64BIT;
	flags &= ~PCI_MSI_FLAGS_MASKBIT;
	cdns_pcie_ep_fn_writew(pcie, fn, cap + PCI_MSI_FLAGS, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(cdns_pcie_ep_set_msi);

int cdns_pcie_ep_get_msi(struct pci_epc *epc, u8 fn, u8 vfn)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	u32 cap = CDNS_PCIE_EP_FUNC_MSI_CAP_OFFSET;
	u16 flags, mme;

	fn = cdns_pcie_get_fn_from_vfn(pcie, fn, vfn);

	/* Validate that the MSI feature is actually enabled. */
	flags = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_FLAGS);
	if (!(flags & PCI_MSI_FLAGS_ENABLE))
		return -EINVAL;

	/*
	 * Get the Multiple Message Enable bitfield from the Message Control
	 * register.
	 */
	mme = FIELD_GET(PCI_MSI_FLAGS_QSIZE, flags);

	return mme;
}
EXPORT_SYMBOL_GPL(cdns_pcie_ep_get_msi);

int cdns_pcie_ep_get_msix(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	u32 cap = CDNS_PCIE_EP_FUNC_MSIX_CAP_OFFSET;
	u32 val, reg;

	func_no = cdns_pcie_get_fn_from_vfn(pcie, func_no, vfunc_no);

	reg = cap + PCI_MSIX_FLAGS;
	val = cdns_pcie_ep_fn_readw(pcie, func_no, reg);
	if (!(val & PCI_MSIX_FLAGS_ENABLE))
		return -EINVAL;

	val &= PCI_MSIX_FLAGS_QSIZE;

	return val;
}
EXPORT_SYMBOL_GPL(cdns_pcie_ep_get_msix);

int cdns_pcie_ep_set_msix(struct pci_epc *epc, u8 fn, u8 vfn,
			  u16 interrupts, enum pci_barno bir,
			  u32 offset)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	struct cdns_pcie *pcie = &ep->pcie;
	u32 cap = CDNS_PCIE_EP_FUNC_MSIX_CAP_OFFSET;
	u32 val, reg;

	fn = cdns_pcie_get_fn_from_vfn(pcie, fn, vfn);

	reg = cap + PCI_MSIX_FLAGS;
	val = cdns_pcie_ep_fn_readw(pcie, fn, reg);
	val &= ~PCI_MSIX_FLAGS_QSIZE;
	val |= interrupts;
	cdns_pcie_ep_fn_writew(pcie, fn, reg, val);

	/* Set MSI-X BAR and offset */
	reg = cap + PCI_MSIX_TABLE;
	val = offset | bir;
	cdns_pcie_ep_fn_writel(pcie, fn, reg, val);

	/* Set PBA BAR and offset.  BAR must match MSI-X BAR */
	reg = cap + PCI_MSIX_PBA;
	val = (offset + (interrupts * PCI_MSIX_ENTRY_SIZE)) | bir;
	cdns_pcie_ep_fn_writel(pcie, fn, reg, val);

	return 0;
}
EXPORT_SYMBOL_GPL(cdns_pcie_ep_set_msix);

int cdns_pcie_ep_map_msi_irq(struct pci_epc *epc, u8 fn, u8 vfn,
			     phys_addr_t addr, u8 interrupt_num,
			     u32 entry_size, u32 *msi_data,
			     u32 *msi_addr_offset)
{
	struct cdns_pcie_ep *ep = epc_get_drvdata(epc);
	u32 cap = CDNS_PCIE_EP_FUNC_MSI_CAP_OFFSET;
	struct cdns_pcie *pcie = &ep->pcie;
	u64 pci_addr, pci_addr_mask = 0xff;
	u16 flags, mme, data, data_mask;
	u8 msi_count;
	int ret;
	int i;

	fn = cdns_pcie_get_fn_from_vfn(pcie, fn, vfn);

	/* Check whether the MSI feature has been enabled by the PCI host. */
	flags = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_FLAGS);
	if (!(flags & PCI_MSI_FLAGS_ENABLE))
		return -EINVAL;

	/* Get the number of enabled MSIs */
	mme = FIELD_GET(PCI_MSI_FLAGS_QSIZE, flags);
	msi_count = 1 << mme;
	if (!interrupt_num || interrupt_num > msi_count)
		return -EINVAL;

	/* Compute the data value to be written. */
	data_mask = msi_count - 1;
	data = cdns_pcie_ep_fn_readw(pcie, fn, cap + PCI_MSI_DATA_64);
	data = data & ~data_mask;

	/* Get the PCI address where to write the data into. */
	pci_addr = cdns_pcie_ep_fn_readl(pcie, fn, cap + PCI_MSI_ADDRESS_HI);
	pci_addr <<= 32;
	pci_addr |= cdns_pcie_ep_fn_readl(pcie, fn, cap + PCI_MSI_ADDRESS_LO);
	pci_addr &= GENMASK_ULL(63, 2);

	for (i = 0; i < interrupt_num; i++) {
		ret = epc->ops->map_addr(epc, fn, vfn, addr,
					 pci_addr & ~pci_addr_mask,
					 entry_size);
		if (ret)
			return ret;
		addr = addr + entry_size;
	}

	*msi_data = data;
	*msi_addr_offset = pci_addr & pci_addr_mask;

	return 0;
}
EXPORT_SYMBOL_GPL(cdns_pcie_ep_map_msi_irq);

static const struct pci_epc_features cdns_pcie_epc_vf_features = {
	.linkup_notifier = false,
	.msi_capable = true,
	.msix_capable = true,
	.align = 65536,
};

static const struct pci_epc_features cdns_pcie_epc_features = {
	.linkup_notifier = false,
	.msi_capable = true,
	.msix_capable = true,
	.align = 256,
};

const struct pci_epc_features*
cdns_pcie_ep_get_features(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	if (!vfunc_no)
		return &cdns_pcie_epc_features;

	return &cdns_pcie_epc_vf_features;
}
EXPORT_SYMBOL_GPL(cdns_pcie_ep_get_features);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cadence PCIe controller driver");
