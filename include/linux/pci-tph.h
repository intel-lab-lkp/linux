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

/*
 * According to the ECN for PCI Firmware Spec, Steering Tag can be different
 * depending on the memory type: Volatile Memory or Persistent Memory. When a
 * caller query about a target's Steering Tag, it must provide the target's
 * tph_mem_type. ECN link: https://members.pcisig.com/wg/PCI-SIG/document/15470.
 * Add a new tph type for PCI peer-to-peer access use case.
 */
enum tph_mem_type {
	TPH_MEM_TYPE_VM,	/* volatile memory */
	TPH_MEM_TYPE_PM,	/* persistent memory */
	TPH_MEM_TYPE_P2P	/* peer-to-peer accessable memory */
};

#ifdef CONFIG_PCIE_TPH
int pcie_tph_set_st_entry(struct pci_dev *pdev,
			  unsigned int index, u16 tag);
int pcie_tph_get_cpu_st(struct pci_dev *dev,
			enum tph_mem_type mem_type,
			unsigned int cpu_uid, u16 *tag);
void pcie_disable_tph(struct pci_dev *pdev);
int pcie_enable_tph(struct pci_dev *pdev, int mode);
u16 pcie_tph_get_st_table_size(struct pci_dev *pdev);
#else
static inline int pcie_tph_set_st_entry(struct pci_dev *pdev,
					unsigned int index, u16 tag)
{ return -EINVAL; }
static inline int pcie_tph_get_cpu_st(struct pci_dev *dev,
				      enum tph_mem_type mem_type,
				      unsigned int cpu_uid, u16 *tag)
{ return -EINVAL; }
static inline void pcie_disable_tph(struct pci_dev *pdev) { }
static inline int pcie_enable_tph(struct pci_dev *pdev, int mode)
{ return -EINVAL; }
#endif

#endif /* LINUX_PCI_TPH_H */
