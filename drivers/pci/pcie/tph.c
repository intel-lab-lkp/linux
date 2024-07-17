// SPDX-License-Identifier: GPL-2.0
/*
 * TPH (TLP Processing Hints) support
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *     Eric Van Tassell <Eric.VanTassell@amd.com>
 *     Wei Huang <wei.huang2@amd.com>
 */

#include <linux/pci.h>
#include <linux/bitfield.h>
#include <linux/pci-tph.h>

#include "../pci.h"

/* Update the ST Mode Select field of TPH Control Register */
static void set_ctrl_reg_mode_sel(struct pci_dev *pdev, u8 st_mode)
{
	u32 reg_val;

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, &reg_val);

	reg_val &= ~PCI_TPH_CTRL_MODE_SEL_MASK;
	reg_val |= FIELD_PREP(PCI_TPH_CTRL_MODE_SEL_MASK, st_mode);

	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, reg_val);
}

/* Update the TPH Requester Enable field of TPH Control Register */
static void set_ctrl_reg_req_en(struct pci_dev *pdev, u8 req_type)
{
	u32 reg_val;

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, &reg_val);

	reg_val &= ~PCI_TPH_CTRL_REQ_EN_MASK;
	reg_val |= FIELD_PREP(PCI_TPH_CTRL_REQ_EN_MASK, req_type);

	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, reg_val);
}

static bool int_vec_mode_supported(struct pci_dev *pdev)
{
	u32 reg_val;
	u8 mode;

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg_val);
	mode = FIELD_GET(PCI_TPH_CAP_INT_VEC, reg_val);

	return !!mode;
}

void pcie_tph_set_nostmode(struct pci_dev *pdev)
{
	if (!pdev->tph_cap)
		return;

	set_ctrl_reg_mode_sel(pdev, PCI_TPH_NO_ST_MODE);
	set_ctrl_reg_req_en(pdev, PCI_TPH_REQ_TPH_ONLY);
}

void pcie_tph_disable(struct pci_dev *pdev)
{
	if (!pdev->tph_cap)
		return;

	set_ctrl_reg_req_en(pdev, PCI_TPH_REQ_DISABLE);
}

void pcie_tph_init(struct pci_dev *pdev)
{
	pdev->tph_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_TPH);
}

/**
 * pcie_tph_intr_vec_supported() - Check if interrupt vector mode supported for dev
 * @pdev: pci device
 *
 * Return:
 *        true : intr vector mode supported
 *        false: intr vector mode not supported
 */
bool pcie_tph_intr_vec_supported(struct pci_dev *pdev)
{
	if (!pdev->tph_cap || pci_tph_disabled() || !pdev->msix_enabled ||
	    !int_vec_mode_supported(pdev))
		return false;

	return true;
}
EXPORT_SYMBOL(pcie_tph_intr_vec_supported);
