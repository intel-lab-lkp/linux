// SPDX-License-Identifier: GPL-2.0-only
/*
 * Persistent-Across-Kexec memory (prmem) - Regions and Region Pools.
 *
 * Copyright (C) 2023 Microsoft Corporation
 * Author: Madhavan T. Venkataraman (madvenka@linux.microsoft.com)
 */
#include <linux/prmem.h>

bool prmem_create_pool(struct prmem_region *region, bool new_region)
{
	size_t		chunk_size, total_size;

	chunk_size = gen_pool_chunk_size(region->size, PAGE_SHIFT);
	total_size = sizeof(*region) + chunk_size;
	total_size = ALIGN(total_size, PAGE_SIZE);

	if (new_region) {
		/*
		 * We place the region structure at the base of the region
		 * itself. Part of the region is a genpool chunk that is used
		 * to manage the region memory.
		 *
		 * Normally, the chunk is allocated from regular memory by
		 * genpool. But in the case of prmem, the chunk must be
		 * persisted across kexecs so allocations can be remembered.
		 * That is why it is allocated from the region memory itself
		 * and passed to genpool.
		 *
		 * Make sure there is enough space for the region and the chunk.
		 */
		if (total_size >= region->size) {
			pr_warn("%s: region size too small\n", __func__);
			return false;
		}

		/* Initialize the persistent genpool chunk. */
		region->chunk = (void *) (region + 1);
		memset(region->chunk, 0, chunk_size);
		gen_pool_init_chunk(region->chunk, (unsigned long) region,
				    region->pa, region->size, true, NULL);
	}

	region->pool = gen_pool_create(PAGE_SHIFT, NUMA_NO_NODE);
	if (!region->pool) {
		pr_warn("%s: Could not create genpool\n", __func__);
		return false;
	}

	gen_pool_add_chunk(region->pool, region->chunk);

	if (new_region) {
		/* Reserve the region and chunk. */
		gen_pool_alloc(region->pool, total_size);
	}
	return true;
}

void *prmem_alloc_pool(struct prmem_region *region, size_t size, int align)
{
	struct genpool_data_align	data = { .align = align, };

	return (void *) gen_pool_alloc_algo(region->pool, size,
					    gen_pool_first_fit_align, &data);
}

void prmem_free_pool(struct prmem_region *region, void *va, size_t size)
{
	gen_pool_free(region->pool, (unsigned long) va, size);
}

struct prmem_region *prmem_add_region(unsigned long pa, size_t size)
{
	struct prmem_region	*region;

	/* Allocate region structure from the base of the region itself. */
	region = __va(pa);
	region->pa = pa;
	region->size = size;

	if (!prmem_create_pool(region, true))
		return NULL;

	list_add_tail(&region->node, &prmem->regions);
	prmem->cur_size += size;
	return region;
}
