// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express Scalable I/O Virtualization (SIOV) support
 */

#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/export.h>
#include "pci.h"

static int pci_siov_sdi_bus(struct pci_dev *dev, int sdi_id)
{
	if (!dev->siov)
		return -EINVAL;
	return pci_virtfn_routing_id(dev, dev->siov->offset,
				  dev->siov->stride, sdi_id) >> 8;
}

static int compute_max_sdi_buses(struct pci_dev *dev)
{
	struct pci_siov *siov = dev->siov;

	if (!siov->offset || (siov->total_SDIs > 1 && !siov->stride))
		return -EIO;

	siov->max_SDI_buses = pci_siov_sdi_bus(dev, siov->total_SDIs - 1);
	return 0;
}

static int siov_init(struct pci_dev *dev, int pos)
{
	struct pci_siov *siov;
	bool was_physfn;
	u16 total;
	u8 status;
	int rc;

	pci_read_config_byte(dev, pos + PCI_SIOV_STATUS, &status);
	if (status & PCI_SIOV_STATUS_ENABLED)
		pci_warn(dev, "SIOV: SDIs active at init, FLR may be required\n");

	pci_read_config_word(dev, pos + PCI_SIOV_TOTAL_SDI, &total);
	if (!total)
		return 0;

	siov = kzalloc_obj(*siov);
	if (!siov)
		return -ENOMEM;

	siov->pos = pos;
	siov->total_SDIs = total;
	siov->driver_max_SDIs = total;
	siov->self = dev;
	pci_read_config_dword(dev, pos + PCI_SIOV_CAP, &siov->cap);
	pci_read_config_word(dev, pos + PCI_SIOV_SDI_OFFSET, &siov->offset);
	pci_read_config_word(dev, pos + PCI_SIOV_SDI_STRIDE, &siov->stride);

	was_physfn = dev->is_physfn;

	dev->siov = siov;
	dev->is_physfn = 1;
	dev->is_siov = 1;
	rc = compute_max_sdi_buses(dev);
	if (rc) {
		dev->siov = NULL;
		dev->is_siov = 0;
		if (!was_physfn)
			dev->is_physfn = 0;
		kfree(siov);
		return rc;
	}

	return 0;
}

static void siov_release(struct pci_dev *dev)
{
	WARN_ON_ONCE(dev->siov->num_SDIs);

	kfree(dev->siov);
	dev->siov = NULL;
	dev->is_siov = 0;
}

/**
 * pci_siov_init - initialize the Scalable IOV capability
 * @dev: the PCI device
 *
 * Returns 0 on success, or negative on failure.
 */
int pci_siov_init(struct pci_dev *dev)
{
	int pos;

	if (!pci_is_pcie(dev))
		return -ENODEV;

	pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_SIOV);
	if (pos)
		return siov_init(dev, pos);

	return -ENODEV;
}

/**
 * pci_siov_release - release resources used by the SIOV capability
 * @dev: the PCI device
 */
void pci_siov_release(struct pci_dev *dev)
{
	if (dev->siov)
		siov_release(dev);
}
