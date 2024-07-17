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
void pcie_tph_disable(struct pci_dev *dev);
void pcie_tph_set_nostmode(struct pci_dev *dev);
bool pcie_tph_intr_vec_supported(struct pci_dev *dev);
int pcie_tph_get_st_from_acpi(struct pci_dev *dev, unsigned int cpu_acpi_uid,
			      enum tph_mem_type tag_type, u8 req_enable,
			      u16 *tag);
#else
static inline void pcie_tph_disable(struct pci_dev *dev) {}
static inline void pcie_tph_set_nostmode(struct pci_dev *dev) {}
static inline bool pcie_tph_intr_vec_supported(struct pci_dev *dev)
{ return false; }
static inline int pcie_tph_get_st_from_acpi(struct pci_dev *dev, unsigned int cpu_acpi_uid,
					    enum tph_mem_type tag_type, u8 req_enable,
					    u16 *tag)
{ return false; }
#endif

#endif /* LINUX_PCI_TPH_H */
