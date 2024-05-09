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

/*
 * The st_info struct defines the steering tag returned by the firmware _DSM
 * method defined in PCI SIG ECN. The specification is available at:
 * https://members.pcisig.com/wg/PCI-SIG/document/15470.

 * @vm_st_valid:  8 bit tag for volatile memory is valid
 * @vm_xst_valid: 16 bit tag for volatile memory is valid
 * @vm_ignore:    1 => was and will be ignored, 0 => ph should be supplied
 * @vm_st:        8 bit steering tag for volatile mem
 * @vm_xst:       16 bit steering tag for volatile mem
 * @pm_st_valid:  8 bit tag for persistent memory is valid
 * @pm_xst_valid: 16 bit tag for persistent memory is valid
 * @pm_ignore:    1 => was and will be ignore, 0 => ph should be supplied
 * @pm_st:        8 bit steering tag for persistent mem
 * @pm_xst:       16 bit steering tag for persistent mem
 */
union st_info {
	struct {
		u64 vm_st_valid:1,
		vm_xst_valid:1,
		vm_ph_ignore:1,
		rsvd1:5,
		vm_st:8,
		vm_xst:16,
		pm_st_valid:1,
		pm_xst_valid:1,
		pm_ph_ignore:1,
		rsvd2:5,
		pm_st:8,
		pm_xst:16;
	};
	u64 value;
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
