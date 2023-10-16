// SPDX-License-Identifier: GPL-2.0-only
/*
 * Persistent-Across-Kexec memory (prmem) - Regions.
 *
 * Copyright (C) 2023 Microsoft Corporation
 * Author: Madhavan T. Venkataraman (madvenka@linux.microsoft.com)
 */
#include <linux/prmem.h>

struct prmem_region *prmem_add_region(unsigned long pa, size_t size)
{
	struct prmem_region	*region;

	/* Allocate region structure from the base of the region itself. */
	region = __va(pa);
	region->pa = pa;
	region->size = size;

	list_add_tail(&region->node, &prmem->regions);
	return region;
}
