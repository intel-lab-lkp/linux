/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Persistent-Across-Kexec memory (prmem) - Definitions.
 *
 * Copyright (C) 2023 Microsoft Corporation
 * Author: Madhavan T. Venkataraman (madvenka@linux.microsoft.com)
 */
#ifndef _LINUX_PRMEM_H
#define _LINUX_PRMEM_H
/*
 * The prmem feature can be used to persist kernel and user data across kexec
 * reboots in memory for various uses. E.g.,
 *
 *	- Saving cached data. E.g., database caches.
 *	- Saving state. E.g., KVM guest states.
 *	- Saving historical information since the last cold boot such as
 *	  events, logs and journals.
 *	- Saving measurements for integrity checks on the next boot.
 *	- Saving driver data.
 *	- Saving IOMMU mappings.
 *	- Saving MMIO config information.
 *
 * This is useful on systems where there is no non-volatile storage or
 * non-volatile storage is too slow.
 */
#include <linux/types.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/printk.h>

#include <asm-generic/errno.h>
#include <asm/page.h>
#include <asm/setup.h>
/*
 * A prmem region supplies the memory for storing persistent data.
 *
 * node		List node.
 * pa		Physical address of the region.
 * size		Size of the region in bytes.
 */
struct prmem_region {
	struct list_head	node;
	unsigned long		pa;
	size_t			size;
};

/*
 * PRMEM metadata.
 *
 * metadata	Physical address of the metadata page.
 * size		Size of initial memory allocated to prmem.
 *
 * regions	List of memory regions.
 */
struct prmem {
	unsigned long		metadata;
	size_t			size;

	/* Persistent Regions. */
	struct list_head	regions;
};

extern struct prmem		*prmem;
extern unsigned long		prmem_metadata;
extern unsigned long		prmem_pa;
extern size_t			prmem_size;

/* Kernel API. */
void prmem_reserve(void);
void prmem_init(void);

/* Internal functions. */
struct prmem_region *prmem_add_region(unsigned long pa, size_t size);

#endif /* _LINUX_PRMEM_H */
