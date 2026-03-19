// SPDX-License-Identifier: GPL-2.0
/*
 * Common EFI memory map functions.
 */

#define pr_fmt(fmt) "efi: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/efi.h>
#include <linux/io.h>
#include <asm/early_ioremap.h>
#include <asm/efi.h>
#include <linux/memblock.h>
#include <linux/slab.h>

static phys_addr_t __init __efi_memmap_alloc_early(unsigned long size)
{
	return memblock_phys_alloc(size, SMP_CACHE_BYTES);
}

static phys_addr_t __init __efi_memmap_alloc_late(unsigned long size)
{
	unsigned int order = get_order(size);
	struct page *p = alloc_pages(GFP_KERNEL, order);

	if (!p)
		return 0;

	return PFN_PHYS(page_to_pfn(p));
}

static
void __init __efi_memmap_free(u64 phys, unsigned long size, unsigned long flags)
{
	if (flags & EFI_MEMMAP_MEMBLOCK) {
		if (slab_is_available())
			memblock_free_late(phys, size);
		else
			memblock_phys_free(phys, size);
	} else if (flags & EFI_MEMMAP_SLAB) {
		struct page *p = pfn_to_page(PHYS_PFN(phys));
		unsigned int order = get_order(size);

		__free_pages(p, order);
	}
}

/**
 * efi_memmap_alloc - Allocate memory for the EFI memory map
 * @num_entries: Number of entries in the allocated map.
 * @data: efi memmap installation parameters
 *
 * Depending on whether mm_init() has already been invoked or not,
 * either memblock or "normal" page allocation is used.
 *
 * Returns zero on success, a negative error code on failure.
 */
int __init efi_memmap_alloc(unsigned int num_entries,
		struct efi_memory_map_data *data)
{
	/* Expect allocation parameters are zero initialized */
	WARN_ON(data->phys_map || data->size);

	data->size = num_entries * efi.memmap.desc_size;
	data->desc_version = efi.memmap.desc_version;
	data->desc_size = efi.memmap.desc_size;
	data->flags &= ~(EFI_MEMMAP_SLAB | EFI_MEMMAP_MEMBLOCK);
	data->flags |= efi.memmap.flags & EFI_MEMMAP_LATE;

	if (slab_is_available()) {
		data->flags |= EFI_MEMMAP_SLAB;
		data->phys_map = __efi_memmap_alloc_late(data->size);
	} else {
		data->flags |= EFI_MEMMAP_MEMBLOCK;
		data->phys_map = __efi_memmap_alloc_early(data->size);
	}

	if (!data->phys_map)
		return -ENOMEM;
	return 0;
}

/**
 * efi_memmap_install - Install a new EFI memory map in efi.memmap
 * @data: efi memmap installation parameters
 *
 * Unlike efi_memmap_init_*(), this function does not allow the caller
 * to switch from early to late mappings. It simply uses the existing
 * mapping function and installs the new memmap.
 *
 * Returns zero on success, a negative error code on failure.
 */
int __init efi_memmap_install(struct efi_memory_map_data *data)
{
	unsigned long size = efi.memmap.map_end - efi.memmap.map;
	unsigned long flags = efi.memmap.flags;
	u64 phys = efi.memmap.phys_map;
	int ret;

	efi_memmap_unmap();

	if (efi_enabled(EFI_PARAVIRT))
		return 0;

	ret = __efi_memmap_init(data);
	if (ret)
		return ret;

	__efi_memmap_free(phys, size, flags);
	return 0;
}
