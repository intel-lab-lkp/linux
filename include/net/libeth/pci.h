/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2025 Intel Corporation */

#ifndef __LIBETH_PCI_H
#define __LIBETH_PCI_H

#include <linux/pci.h>

/**
 * struct libeth_pci_mmio_region - structure for MMIO region info
 * @list: used to add a MMIO region to the list of MMIO regions in
 *	  libeth_mmio_info
 * @addr: virtual address of MMIO region start
 * @offset: start offset of the MMIO region
 * @size: size of the MMIO region
 * @bar_idx: BAR index to which the MMIO region belongs to
 */
struct libeth_pci_mmio_region {
	struct list_head	list;
	void __iomem		*addr;
	resource_size_t		offset;
	resource_size_t		size;
	u16			bar_idx;
};

/**
 * struct libeth_mmio_info - contains list of MMIO regions
 * @pdev: PCI device pointer
 * @mmio_list: list of MMIO regions
 */
struct libeth_mmio_info {
	struct pci_dev		*pdev;
	struct list_head	mmio_list;
};

#define libeth_pci_map_mmio_region(mmio_info, offset, size, ...)	\
	__libeth_pci_map_mmio_region(mmio_info, offset, size,		\
				     COUNT_ARGS(__VA_ARGS__), ##__VA_ARGS__)

#define libeth_pci_get_mmio_addr(mmio_info, offset, ...)		\
	__libeth_pci_get_mmio_addr(mmio_info, offset,			\
				   COUNT_ARGS(__VA_ARGS__), ##__VA_ARGS__)

bool __libeth_pci_map_mmio_region(struct libeth_mmio_info *mmio_info,
				  resource_size_t offset,
				  resource_size_t size,
				  int num_args, ...);
void __iomem *__libeth_pci_get_mmio_addr(struct libeth_mmio_info *mmio_info,
					 resource_size_t region_offset,
					 int num_args, ...);
void libeth_pci_unmap_all_mmio_regions(struct libeth_mmio_info *mmio_info);
int libeth_pci_init_dev(struct pci_dev *pdev);
void libeth_pci_deinit_dev(struct pci_dev *pdev);

#endif /* __LIBETH_PCI_H */
