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
 */
enum tph_mem_type {
	TPH_MEM_TYPE_VM,	/* volatile memory */
	TPH_MEM_TYPE_PM		/* persistent memory */
};

/*
 * TPH requester selection policy
 * Used to select standard/extended TPH type for TPH enable and
 * Steering Tag retrieval.
 */
enum tph_req_policy {
	TPH_REQ_AUTO,		/* Kernel auto selection based on capability */
	TPH_REQ_STANDARD,	/* Force standard TPH (8-bit ST) */
	TPH_REQ_EXTENDED	/* Force extended TPH (16-bit ST) */
};

#ifdef CONFIG_PCIE_TPH
int pcie_tph_set_st_entry(struct pci_dev *pdev,
			  unsigned int index, u16 tag);
int pcie_tph_get_cpu_st(struct pci_dev *dev,
			enum tph_mem_type mem_type,
			enum tph_req_policy req_policy,
			unsigned int cpu, u16 *tag);
void pcie_disable_tph(struct pci_dev *pdev);
int pcie_enable_tph(struct pci_dev *pdev, int mode,
		    enum tph_req_policy req_policy);
u16 pcie_tph_get_st_table_size(struct pci_dev *pdev);
u32 pcie_tph_get_st_table_loc(struct pci_dev *pdev);
#else
static inline int pcie_tph_set_st_entry(struct pci_dev *pdev,
					unsigned int index, u16 tag)
{ return -EINVAL; }
static inline int pcie_tph_get_cpu_st(struct pci_dev *dev,
				      enum tph_mem_type mem_type,
				      enum tph_req_policy req_policy,
				      unsigned int cpu, u16 *tag)
{ return -EINVAL; }
static inline void pcie_disable_tph(struct pci_dev *pdev) { }
static inline int pcie_enable_tph(struct pci_dev *pdev, int mode,
				  enum tph_req_policy req_policy)
{ return -EINVAL; }
static inline u16 pcie_tph_get_st_table_size(struct pci_dev *pdev)
{ return 0; }
static inline u32 pcie_tph_get_st_table_loc(struct pci_dev *pdev)
{ return 0; }
#endif

#endif /* LINUX_PCI_TPH_H */
