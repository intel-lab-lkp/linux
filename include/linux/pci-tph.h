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
#else
static inline void pcie_tph_disable(struct pci_dev *dev) {}
#endif

#endif /* LINUX_PCI_TPH_H */
