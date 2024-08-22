// SPDX-License-Identifier: GPL-2.0
/*
 * TPH (TLP Processing Hints) support
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *     Eric Van Tassell <Eric.VanTassell@amd.com>
 *     Wei Huang <wei.huang2@amd.com>
 */
#include <linux/pci.h>
#include <linux/pci-tph.h>

#include "../pci.h"

static u8 get_st_modes(struct pci_dev *pdev)
{
	u32 reg;

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg);
	reg &= PCI_TPH_CAP_NO_ST | PCI_TPH_CAP_INT_VEC | PCI_TPH_CAP_DEV_SPEC;

	return reg;
}

/**
 * pcie_tph_modes - Get the ST modes supported by device
 * @pdev: PCI device
 *
 * Returns a bitmask with all TPH modes supported by a device as shown in the
 * TPH capability register. Current supported modes include:
 *   PCI_TPH_CAP_NO_ST - NO ST Mode Supported
 *   PCI_TPH_CAP_INT_VEC - Interrupt Vector Mode Supported
 *   PCI_TPH_CAP_DEV_SPEC - Device Specific Mode Supported
 *
 * Return: 0 when TPH is not supported, otherwise bitmask of supported modes
 */
int pcie_tph_modes(struct pci_dev *pdev)
{
	if (!pdev->tph_cap)
		return 0;

	return get_st_modes(pdev);
}
EXPORT_SYMBOL(pcie_tph_modes);

void pci_tph_init(struct pci_dev *pdev)
{
	pdev->tph_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_TPH);
}
