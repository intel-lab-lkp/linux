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
#include <linux/genalloc.h>
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
 * pool		Gen Pool to manage region memory.
 * chunk	Persistent Gen Pool chunk.
 */
struct prmem_region {
	struct list_head	node;
	unsigned long		pa;
	size_t			size;
	struct gen_pool		*pool;
	struct gen_pool_chunk	*chunk;
};

#define PRMEM_MAX_CACHES	14

/*
 * PRMEM metadata.
 *
 * checksum	Just before reboot, a checksum is computed on the metadata. On
 *		the next kexec reboot, the metadata is validated with the
 *		checksum to make sure that the metadata has not been corrupted.
 * metadata	Physical address of the metadata page.
 * size		Size of initial memory allocated to prmem.
 *
 * regions	List of memory regions.
 *
 * caches	Caches for different object sizes. For allocations smaller than
 *		PAGE_SIZE, these caches are used.
 */
struct prmem {
	unsigned long		checksum;
	unsigned long		metadata;
	size_t			size;

	/* Persistent Regions. */
	struct list_head	regions;

	/* Allocation caches. */
	void			*caches[PRMEM_MAX_CACHES];
};

extern struct prmem		*prmem;
extern unsigned long		prmem_metadata;
extern unsigned long		prmem_pa;
extern size_t			prmem_size;
extern bool			prmem_inited;
extern spinlock_t		prmem_lock;

/* Kernel API. */
void prmem_reserve_early(void);
void prmem_reserve(void);
void prmem_init(void);
void prmem_fini(void);
int  prmem_cmdline_size(void);

/* Allocator API. */
struct page *prmem_alloc_pages(unsigned int order, gfp_t gfp);
void prmem_free_pages(struct page *pages, unsigned int order);
void *prmem_alloc(size_t size, gfp_t gfp);
void prmem_free(void *va, size_t size);

/* Internal functions. */
struct prmem_region *prmem_add_region(unsigned long pa, size_t size);
bool prmem_create_pool(struct prmem_region *region, bool new_region);
void *prmem_alloc_pool(struct prmem_region *region, size_t size, int align);
void prmem_free_pool(struct prmem_region *region, void *va, size_t size);
void *prmem_alloc_pages_locked(unsigned int order);
void prmem_free_pages_locked(void *va, unsigned int order);
void *prmem_alloc_locked(size_t size);
void prmem_free_locked(void *va, size_t size);
unsigned long prmem_checksum(void *start, size_t size);
bool __init prmem_validate(void);
void prmem_cmdline(char *cmdline);

#endif /* _LINUX_PRMEM_H */
