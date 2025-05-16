// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2025 Intel Corporation */

#include <linux/intel/libie/pci.h>

/**
 * libie_find_mmio_region - find if MMIO region is present in the list
 * @mmio_list: list that contains MMIO region info
 * @offset: MMIO region start offset
 * @bar_idx: BAR index where the offset to search
 *
 * Return: MMIO region pointer or NULL if the region info is not present.
 */
static struct libie_pci_mmio_region *
libie_find_mmio_region(const struct list_head *mmio_list,
		       resource_size_t offset, int bar_idx)
{
	struct libie_pci_mmio_region *mr;

	list_for_each_entry(mr, mmio_list, list)
		if (mr->bar_idx == bar_idx && mr->offset == offset)
			return mr;

	return NULL;
}

/**
 * __libie_pci_get_mmio_addr - get the MMIO virtual address
 * @mmio_info: contains list of MMIO regions
 * @offset: register offset of find
 * @num_args: number of additional arguments present
 *
 * This function finds the virtual address of a register offset by iterating
 * through the non-linear MMIO regions that are mapped by the driver.
 *
 * Return: valid MMIO virtual address or NULL.
 */
void __iomem *__libie_pci_get_mmio_addr(struct libie_mmio_info *mmio_info,
					resource_size_t offset,
					int num_args, ...)
{
	struct libie_pci_mmio_region *mr;
	int bar_idx = 0;
	va_list args;

	if (num_args) {
		va_start(args, num_args);
		bar_idx = va_arg(args, int);
		va_end(args);
	}

	list_for_each_entry(mr, &mmio_info->mmio_list, list)
		if (bar_idx == mr->bar_idx && offset >= mr->offset &&
		    offset < mr->offset + mr->size) {
			offset -= mr->offset;

			return mr->addr + offset;
		}

	return NULL;
}
EXPORT_SYMBOL_NS_GPL(__libie_pci_get_mmio_addr, "LIBIE_PCI");

/**
 * __libie_pci_map_mmio_region - map PCI device MMIO region
 * @mmio_info: struct to store the mapped MMIO region
 * @offset: MMIO region start offset
 * @size: MMIO region size
 * @num_args: number of additional arguments present
 *
 * Return: true on success, false on memory map failure.
 */
bool __libie_pci_map_mmio_region(struct libie_mmio_info *mmio_info,
				 resource_size_t offset,
				 resource_size_t size, int num_args, ...)
{
	struct pci_dev *pdev = mmio_info->pdev;
	struct libie_pci_mmio_region *mr;
	resource_size_t pa;
	void __iomem *va;
	int bar_idx = 0;
	va_list args;

	if (num_args) {
		va_start(args, num_args);
		bar_idx = va_arg(args, int);
		va_end(args);
	}

	mr = libie_find_mmio_region(&mmio_info->mmio_list, offset, bar_idx);
	if (mr) {
		pci_warn(pdev, "Mapping of BAR%u with offset %llu already exists\n",
			 bar_idx, (unsigned long long)offset);
		return true;
	}

	pa = pci_resource_start(pdev, bar_idx) + offset;
	va = ioremap(pa, size);
	if (!va) {
		pci_err(pdev, "Failed to allocate BAR%u region\n", bar_idx);
		return false;
	}

	mr = kvzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr) {
		iounmap(va);
		return false;
	}

	mr->addr = va;
	mr->offset = offset;
	mr->size = size;
	mr->bar_idx = bar_idx;

	list_add_tail(&mr->list, &mmio_info->mmio_list);

	return true;
}
EXPORT_SYMBOL_NS_GPL(__libie_pci_map_mmio_region, "LIBIE_PCI");

/**
 * libie_pci_unmap_all_mmio_regions - unmap all PCI device MMIO regions
 * @mmio_info: contains list of MMIO regions to unmap
 */
void libie_pci_unmap_all_mmio_regions(struct libie_mmio_info *mmio_info)
{
	struct libie_pci_mmio_region *mr, *tmp;

	list_for_each_entry_safe(mr, tmp, &mmio_info->mmio_list, list) {
		iounmap(mr->addr);
		list_del(&mr->list);
		kfree(mr);
	}
}
EXPORT_SYMBOL_NS_GPL(libie_pci_unmap_all_mmio_regions, "LIBIE_PCI");

/**
 * libie_pci_init_dev - enable and reserve PCI regions of the device
 * @pdev: PCI device information
 *
 * Return: %0 on success, -%errno on failure.
 */
int libie_pci_init_dev(struct pci_dev *pdev)
{
	int err;

	err = pci_enable_device(pdev);
	if (err)
		return err;

	err = pci_request_mem_regions(pdev, pci_name(pdev));
	if (err)
		goto disable_dev;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err)
		goto rel_regions;

	pci_set_master(pdev);

	return 0;

rel_regions:
	pci_release_regions(pdev);
disable_dev:
	pci_disable_device(pdev);

	return err;
}
EXPORT_SYMBOL_NS_GPL(libie_pci_init_dev, "LIBIE_PCI");

/**
 * libie_pci_deinit_dev - disable and release the PCI regions of the device
 * @pdev: PCI device information
 */
void libie_pci_deinit_dev(struct pci_dev *pdev)
{
	pci_disable_device(pdev);
	pci_release_regions(pdev);
}
EXPORT_SYMBOL_NS_GPL(libie_pci_deinit_dev, "LIBIE_PCI");

MODULE_DESCRIPTION("Common Ethernet PCI library");
MODULE_LICENSE("GPL");
