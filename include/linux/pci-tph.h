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

enum tph_mem_type {
	TPH_MEM_TYPE_VM,	/* volatile memory type */
	TPH_MEM_TYPE_PM		/* persistent memory type */
};

#ifdef CONFIG_PCIE_TPH
int pcie_tph_disable(struct pci_dev *dev);
int tph_set_dev_nostmode(struct pci_dev *dev);
bool pcie_tph_get_st(struct pci_dev *dev, unsigned int cpu,
		     enum tph_mem_type tag_type, u8 req_enable,
		     u16 *tag);
bool pcie_tph_set_st(struct pci_dev *dev, unsigned int msix_nr,
		     unsigned int cpu, enum tph_mem_type tag_type,
		     u8 req_enable);
#else
static inline int pcie_tph_disable(struct pci_dev *dev)
{ return -EOPNOTSUPP; }
static inline int tph_set_dev_nostmode(struct pci_dev *dev)
{ return -EOPNOTSUPP; }
static inline bool pcie_tph_get_st(struct pci_dev *dev, unsigned int cpu,
				   enum tph_mem_type tag_type, u8 req_enable,
				   u16 *tag)
{ return false; }
static inline bool pcie_tph_set_st(struct pci_dev *dev, unsigned int msix_nr,
				   unsigned int cpu, enum tph_mem_type tag_type,
				   u8 req_enable)
{ return true; }
#endif

#endif /* LINUX_PCI_TPH_H */
