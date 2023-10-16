// SPDX-License-Identifier: GPL-2.0
/*
 * Persistent-Across-Kexec memory (prmem) - Reserve memory.
 *
 * Copyright (C) 2023 Microsoft Corporation
 * Author: Madhavan T. Venkataraman (madvenka@linux.microsoft.com)
 */
#include <linux/prmem.h>

struct prmem		*prmem;
unsigned long		prmem_metadata;
unsigned long		prmem_pa;
unsigned long		prmem_size;
unsigned long		prmem_max_size;

void __init prmem_reserve_early(void)
{
	struct prmem_region	*region;
	unsigned long		nregions;

	/* Need to specify an initial size to enable prmem. */
	if (!prmem_size)
		return;

	/* Nothing to be done if it is a cold boot. */
	if (!prmem_metadata)
		return;

	/*
	 * prmem uses direct map addresses. If PAGE_OFFSET is randomized,
	 * these addresses will change across kexecs. Persistence cannot
	 * be supported.
	 */
	if (kaslr_memory_enabled()) {
		pr_warn("%s: Cannot support persistence because of KASLR.\n",
			__func__);
		return;
	}

	/*
	 * This is a kexec reboot. If any step fails here, treat this like a
	 * cold boot. That is, forget all persistent data and start over.
	 */

	/* Reserve metadata page. */
	if (memblock_reserve(prmem_metadata, PAGE_SIZE)) {
		pr_warn("%s: Unable to reserve metadata at %lx\n", __func__,
			prmem_metadata);
		return;
	}
	prmem = __va(prmem_metadata);

	/* Make sure that the metadata is sane. */
	if (!prmem_validate())
		goto unreserve_metadata;

	/* Reserve regions that were added to prmem. */
	nregions = 0;
	list_for_each_entry(region, &prmem->regions, node) {
		if (memblock_reserve(region->pa, region->size)) {
			pr_warn("%s: Unable to reserve %lx, %lx\n", __func__,
				region->pa, region->size);
			goto unreserve_regions;
		}
		nregions++;
	}
	return;

unreserve_regions:
	/* Unreserve regions. */
	list_for_each_entry(region, &prmem->regions, node) {
		if (!nregions)
			break;
		memblock_unreserve(region->pa, region->size);
		nregions--;
	}

unreserve_metadata:
	/* Unreserve the metadata page. */
	memblock_unreserve(prmem_metadata, PAGE_SIZE);
	prmem = NULL;
}

void __init prmem_reserve(void)
{
	BUILD_BUG_ON(sizeof(*prmem) > PAGE_SIZE);

	if (!prmem_size || prmem)
		return;

	/*
	 * prmem uses direct map addresses. If PAGE_OFFSET is randomized,
	 * these addresses will change across kexecs. Persistence cannot
	 * be supported.
	 */
	if (kaslr_memory_enabled()) {
		pr_warn("%s: Cannot support persistence because of KASLR.\n",
			__func__);
		return;
	}

	/* Allocate a metadata page. */
	prmem_metadata = memblock_phys_alloc(PAGE_SIZE, PAGE_SIZE);
	if (!prmem_metadata) {
		pr_warn("%s: Could not allocate metadata at %lx\n", __func__,
			prmem_metadata);
		return;
	}

	/* Allocate initial memory. */
	prmem_pa = memblock_phys_alloc(prmem_size, PAGE_SIZE);
	if (!prmem_pa) {
		pr_warn("%s: Could not allocate initial memory\n", __func__);
		goto free_metadata;
	}

	/* Clear metadata. */
	prmem = __va(prmem_metadata);
	memset(prmem, 0, sizeof(*prmem));
	return;

free_metadata:
	memblock_phys_free(prmem_metadata, PAGE_SIZE);
	prmem = NULL;
}
