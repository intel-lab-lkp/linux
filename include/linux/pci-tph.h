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
void pcie_tph_disable(struct pci_dev *dev);
void pcie_tph_set_nostmode(struct pci_dev *dev);
bool pcie_tph_intr_vec_supported(struct pci_dev *dev);
#else
static inline void pcie_tph_disable(struct pci_dev *dev) {}
static inline void pcie_tph_set_nostmode(struct pci_dev *dev) {}
static inline bool pcie_tph_intr_vec_supported(struct pci_dev *dev)
{ return false; }
#endif

#endif /* LINUX_PCI_TPH_H */
