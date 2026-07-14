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

#include <linux/pci.h>

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

#ifdef CONFIG_PCIE_TPH
/**
 * pcie_std_tph_supported - check standard TPH requester support
 * @pdev: PCI device
 */
#define pcie_std_tph_supported(pdev) \
	((pdev)->tph_max_type >= PCI_TPH_REQ_TPH_ONLY)
/**
 * pcie_ext_tph_supported - check extended TPH requester support
 * @pdev: PCI device
 */
#define pcie_ext_tph_supported(pdev) \
	((pdev)->tph_max_type == PCI_TPH_REQ_EXT_TPH)
int pcie_tph_set_st_entry(struct pci_dev *pdev,
			  unsigned int index, u16 tag);
int pcie_tph_set_st_entries(struct pci_dev *pdev, unsigned int start,
			    unsigned int count, const u16 *tags);
int pcie_tph_get_cpu_st_ext(struct pci_dev *dev, enum tph_mem_type mem_type,
			    u8 req_type, unsigned int cpu, u16 *tag);
int pcie_tph_get_cpu_st(struct pci_dev *dev, enum tph_mem_type mem_type,
			unsigned int cpu, u16 *tag);
void pcie_disable_tph(struct pci_dev *pdev);
int pcie_enable_tph_ext(struct pci_dev *pdev, u8 mode, u8 req_type);
int pcie_enable_tph(struct pci_dev *pdev, u8 mode);
u16 pcie_tph_get_st_table_size(struct pci_dev *pdev);
u32 pcie_tph_get_st_table_loc(struct pci_dev *pdev);
u8 pcie_tph_enabled_req_type(struct pci_dev *pdev);
u8 pcie_tph_completer_type(struct pci_dev *pdev);
#else
#define pcie_std_tph_supported(pdev) false
#define pcie_ext_tph_supported(pdev) false
static inline int pcie_tph_set_st_entry(struct pci_dev *pdev,
					unsigned int index, u16 tag)
{ return -EINVAL; }
static inline int pcie_tph_set_st_entries(struct pci_dev *pdev,
					  unsigned int start,
					  unsigned int count, const u16 *tags)
{ return -EINVAL; }
static inline int pcie_tph_get_cpu_st_ext(struct pci_dev *dev,
					  enum tph_mem_type mem_type,
					  u8 req_type, unsigned int cpu,
					  u16 *tag)
{ return -EINVAL; }
static inline int pcie_tph_get_cpu_st(struct pci_dev *dev,
				      enum tph_mem_type mem_type,
				      unsigned int cpu, u16 *tag)
{ return -EINVAL; }
static inline void pcie_disable_tph(struct pci_dev *pdev) { }
static inline int pcie_enable_tph_ext(struct pci_dev *pdev, u8 mode,
				      u8 req_type)
{ return -EINVAL; }
static inline int pcie_enable_tph(struct pci_dev *pdev, u8 mode)
{ return -EINVAL; }
static inline u16 pcie_tph_get_st_table_size(struct pci_dev *pdev)
{ return 0; }
static inline u32 pcie_tph_get_st_table_loc(struct pci_dev *pdev)
{ return PCI_TPH_LOC_NONE; }
static inline u8 pcie_tph_enabled_req_type(struct pci_dev *pdev)
{ return PCI_TPH_REQ_DISABLE; }
static inline u8 pcie_tph_completer_type(struct pci_dev *pdev)
{ return PCI_EXP_DEVCAP2_TPH_COMP_NONE; }
#endif

#endif /* LINUX_PCI_TPH_H */
