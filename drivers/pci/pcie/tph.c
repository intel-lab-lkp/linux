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
#include <linux/msi.h>
#include <linux/pci-acpi.h>

#include "../pci.h"

void pcie_tph_init(struct pci_dev *dev)
{
	dev->tph_cap = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_TPH);
}

