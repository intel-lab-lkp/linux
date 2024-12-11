// SPDX-License-Identifier: GPL-2.0
/*
 * Purpose:	PCI Express P2P link discovery
 *
 * Copyright (C) 2024 Broadcom Inc.
 */

#include <linux/bitfield.h>
#include <linux/pci.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/bitops.h>

#include "../pci.h"
#include "portdrv.h"
#include "p2p_link.h"

/**
 * pcie_brcm_is_p2p_supported - Broadcom device specific handler
 *       to check if the upstream port supports inter switch P2P.
 *
 * @dev: PCIe upstream port to check
 *
 * This function assumes the PCIe upstream port is a Broadcom
 * PCIe device.
 */
static bool pcie_brcm_is_p2p_supported(struct pci_dev *dev)
{
	u64 dsn;
	u16 vsec;
	u32 vsec_data;

	vsec = pci_find_vsec_capability(dev, PCI_VENDOR_ID_LSI_LOGIC,
					PCIE_BRCM_SW_P2P_VSEC_ID);
	if (!vsec) {
		pci_dbg(dev, "Failed to get VSEC capability\n");
		return false;
	}

	pci_read_config_dword(dev, vsec + PCIE_BRCM_SW_P2P_MODE_VSEC_OFFSET,
			      &vsec_data);

	dsn = pci_get_dsn(dev);
	if (!dsn) {
		pci_dbg(dev, "DSN capability is not present\n");
		return false;
	}

	pci_dbg(dev, "Serial Number: 0x%llx VSEC 0x%x\n",
		dsn, vsec_data);

	/* Check if the PEX switch has a valid P2P support */
	if (!(dsn & PCIE_BRCM_SW_DSN_P2P_STATUS))
		return false;

	return FIELD_GET(PCIE_BRCM_SW_P2P_MODE_MASK, vsec_data) ==
		PCIE_BRCM_SW_P2P_MODE_INTER_SW_LINK;
}

/*
 * Determine if device supports Inter switch P2P links.
 *
 * Return value: true if inter switch P2P is supported, return false otherwise.
 */
static bool pcie_port_is_p2p_supported(struct pci_dev *dev)
{
	/* P2P link attribute is supported on upstream ports only */
	if (pci_pcie_type(dev) != PCI_EXP_TYPE_UPSTREAM)
		return false;

	/*
	 * Currently Broadcom PEX switches are supported.
	 */
	if (dev->vendor == PCI_VENDOR_ID_LSI_LOGIC &&
	    (dev->device == PCI_DEVICE_ID_BRCM_PEX_89000_HLC ||
	     dev->device == PCI_DEVICE_ID_BRCM_PEX_89000_LLC))
		return pcie_brcm_is_p2p_supported(dev);

	return false;
}

/*
 * Traverse list of all PCI bridges and find devices that support Inter switch P2P
 * and have the same serial number to create report the BDF over sysfs.
 */
static ssize_t links_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev), *pdev_link = NULL;
	size_t len = 0;
	u64 dsn, dsn_link;

	/*
	 * pdev's DSN has already been verified to be available before creating
	 * the sysfs entry.
	 */
	dsn = pci_get_dsn(pdev);

	/* Traverse list of PCI bridges to determine any available P2P links */
	while ((pdev_link = pci_get_class(PCI_CLASS_BRIDGE_PCI << 8, pdev_link))
			!= NULL) {
		if (pdev_link == pdev)
			continue;

		if (!pcie_port_is_p2p_supported(pdev_link))
			continue;

		dsn_link = pci_get_dsn(pdev_link);
		if (!dsn_link)
			continue;

		if (dsn == dsn_link)
			len += sysfs_emit_at(buf, len, "%04x:%02x:%02x.%d\n",
					     pci_domain_nr(pdev_link->bus),
					     pdev_link->bus->number, PCI_SLOT(pdev_link->devfn),
					     PCI_FUNC(pdev_link->devfn));
	}

	return len;
}

/* P2P link sysfs attribute. */
static struct device_attribute dev_attr_links =
	__ATTR(links, 0444, links_show, NULL);

static struct attribute *pcie_port_p2p_link_attrs[] = {
	&dev_attr_links.attr,
	NULL
};

const struct attribute_group pcie_port_p2p_link_attr_group = {
	.name = "p2p_link",
	.attrs = pcie_port_p2p_link_attrs,
};

void p2p_link_sysfs_update_group(struct pci_dev *pdev)
{
	if (!pcie_port_is_p2p_supported(pdev))
		return;

	sysfs_update_group(&pdev->dev.kobj, &pcie_port_p2p_link_attr_group);
}

/*
 * pcie_port_is_p2p_link_available: Determine if a P2P link is available
 * between the two upstream bridges. The serial number of the two devices
 * will be compared and if they are same then it is considered that the P2P
 * link is available.
 *
 * Return value: true if inter switch P2P is available, return false otherwise.
 */
bool pcie_port_is_p2p_link_available(struct pci_dev *a, struct pci_dev *b)
{
	if (!pcie_port_is_p2p_supported(a) || !pcie_port_is_p2p_supported(b))
		return false;

	/* the above check validates DSN is valid for both devices */
	return pci_get_dsn(a) == pci_get_dsn(b);
}
EXPORT_SYMBOL_GPL(pcie_port_is_p2p_link_available);
