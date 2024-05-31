// SPDX-License-Identifier: GPL-2.0
/*
 * TPH (TLP Processing Hints) support
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *     Eric Van Tassell <Eric.VanTassell@amd.com>
 *     Wei Huang <wei.huang2@amd.com>
 */

#define pr_fmt(fmt) "TPH: " fmt
#define dev_fmt pr_fmt

#include <linux/acpi.h>
#include <uapi/linux/pci_regs.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/msi.h>
#include <linux/pci.h>
#include <linux/pci-tph.h>
#include <linux/msi.h>
#include <linux/pci-acpi.h>

#include "../pci.h"

static int tph_set_reg_field_u32(struct pci_dev *dev, u8 offset, u32 mask,
				 u8 shift, u32 field)
{
	u32 reg_val;
	int ret;

	if (!dev->tph_cap)
		return -EINVAL;

	ret = pci_read_config_dword(dev, dev->tph_cap + offset, &reg_val);
	if (ret)
		return ret;

	reg_val &= ~mask;
	reg_val |= (field << shift) & mask;

	ret = pci_write_config_dword(dev, dev->tph_cap + offset, reg_val);

	return ret;
}

int tph_set_dev_nostmode(struct pci_dev *dev)
{
	int ret;

	/* set ST Mode Select to "No ST Mode" */
	ret = tph_set_reg_field_u32(dev, PCI_TPH_CTRL,
				    PCI_TPH_CTRL_MODE_SEL_MASK,
				    PCI_TPH_CTRL_MODE_SEL_SHIFT,
				    PCI_TPH_NO_ST_MODE);
	if (ret)
		return ret;

	/* set "TPH Requester Enable" to "TPH only" */
	ret = tph_set_reg_field_u32(dev, PCI_TPH_CTRL,
				    PCI_TPH_CTRL_REQ_EN_MASK,
				    PCI_TPH_CTRL_REQ_EN_SHIFT,
				    PCI_TPH_REQ_TPH_ONLY);

	return ret;
}

int pcie_tph_disable(struct pci_dev *dev)
{
	return  tph_set_reg_field_u32(dev, PCI_TPH_CTRL,
				      PCI_TPH_CTRL_REQ_EN_MASK,
				      PCI_TPH_CTRL_REQ_EN_SHIFT,
				      PCI_TPH_REQ_DISABLE);
}

void pcie_tph_init(struct pci_dev *dev)
{
	dev->tph_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_TPH);
}

