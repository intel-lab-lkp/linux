/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TPH (TLP Processing Hints)
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *     Eric Van Tassell <Eric.VanTassell@amd.com>
 *     Wei Huang <wei.huang2@amd.com>
 */
#ifndef LINUX_PCI_TPH_H
#define LINUX_PCI_TPH_H

#ifdef CONFIG_PCIE_TPH
int pcie_tph_modes(struct pci_dev *pdev);
#else
static inline int pcie_tph_modes(struct pci_dev *pdev) { return 0; }
#endif

#endif /* LINUX_PCI_TPH_H */
