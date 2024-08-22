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
void pcie_disable_tph(struct pci_dev *pdev);
int pcie_enable_tph(struct pci_dev *pdev, int mode);
int pcie_tph_modes(struct pci_dev *pdev);
#else
static inline void pcie_disable_tph(struct pci_dev *pdev) { }
static inline int pcie_enable_tph(struct pci_dev *pdev, int mode)
{ return -EINVAL; }
static inline int pcie_tph_modes(struct pci_dev *pdev) { return 0; }
#endif

#endif /* LINUX_PCI_TPH_H */
