// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kpkeys.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/set_memory.h>

#include <kunit/visibility.h>

__ro_after_init DEFINE_STATIC_KEY_FALSE(kpkeys_hardened_pgtables_key);
EXPORT_SYMBOL_IF_KUNIT(kpkeys_hardened_pgtables_key);

static int set_pkey_pgtable(struct page *page, unsigned int nr_pages)
{
	unsigned long addr = (unsigned long)page_address(page);
	int ret;

	ret = set_memory_pkey(addr, nr_pages, KPKEYS_PKEY_PGTABLES);

	WARN_ON(ret);
	return ret;
}

static int set_pkey_default(struct page *page, unsigned int nr_pages)
{
	unsigned long addr = (unsigned long)page_address(page);
	int ret;

	ret = set_memory_pkey(addr, nr_pages, KPKEYS_PKEY_DEFAULT);

	WARN_ON(ret);
	return ret;
}

/* pkeys physmem allocator (PPA) - implemented below */
static void ppa_finalize(void);

struct page *kpkeys_pgtable_alloc(gfp_t gfp, unsigned int order)
{
	struct page *page;
	int ret;

	page = alloc_pages_noprof(gfp, order);
	if (!page)
		return page;

	ret = set_pkey_pgtable(page, 1 << order);
	if (ret) {
		__free_pages(page, order);
		return NULL;
	}

	return page;
}

void kpkeys_pgtable_free(struct page *page, unsigned int order)
{
	set_pkey_default(page, 1 << order);
	__free_pages(page, order);
}

void __init kpkeys_hardened_pgtables_init(void)
{
	if (!kpkeys_enabled())
		return;

	static_branch_enable(&kpkeys_hardened_pgtables_key);

	ppa_finalize();
	arch_kpkeys_protect_static_pgtables();
}

/*
 * pkeys physmem allocator (PPA): allocator for very early page tables
 * (especially for creating the linear map), based on memblock. Allocated
 * ranges are tracked so that their pkey can be set once it is safe to do so.
 */

/*
 * We may have to track many ranges when allocating page tables for the linear
 * map, as their number grows with the amount of available memory. Assuming that
 * memblock returns contiguous blocks whenever possible, the number of ranges
 * to track cannot however exceed the number of regions that memblock itself
 * tracks. memblock_allow_resize() hasn't been called yet at that point, so
 * that limit is the size of the statically allocated array.
 */
#define PHYSMEM_MAX_RANGES	INIT_MEMBLOCK_MEMORY_REGIONS

struct physmem_range {
	phys_addr_t addr;
	phys_addr_t size;
};

struct pkeys_physmem_allocator {
	struct physmem_range allocated_ranges[PHYSMEM_MAX_RANGES];
	unsigned int nr_allocated_ranges;
};

static struct pkeys_physmem_allocator pkeys_physmem_allocator __initdata;

static int __init set_pkey_pgtable_phys(phys_addr_t pa, phys_addr_t size)
{
	unsigned long addr = (unsigned long)__va(pa);
	int ret;

	ret = set_memory_pkey(addr, size / PAGE_SIZE, KPKEYS_PKEY_PGTABLES);

	WARN_ON(ret);
	return ret;
}

static bool __init ppa_try_extend_last_range(phys_addr_t addr, phys_addr_t size)
{
	struct pkeys_physmem_allocator *ppa = &pkeys_physmem_allocator;
	struct physmem_range *range;

	if (!ppa->nr_allocated_ranges)
		return false;

	range = &ppa->allocated_ranges[ppa->nr_allocated_ranges - 1];

	/* Merge the new range into the last range if they are contiguous */
	if (addr == range->addr + range->size) {
		range->size += size;
		return true;
	} else if (addr + size == range->addr) {
		range->addr -= size;
		range->size += size;
		return true;
	}

	return false;
}

static void __init ppa_register_allocated_range(phys_addr_t addr,
						phys_addr_t size)
{
	struct pkeys_physmem_allocator *ppa = &pkeys_physmem_allocator;
	struct physmem_range *range;

	if (!addr)
		return;

	if (ppa_try_extend_last_range(addr, size))
		return;

	/* Could not extend the last range, create a new one */
	if (WARN_ON(ppa->nr_allocated_ranges >= PHYSMEM_MAX_RANGES))
		return;

	range = &ppa->allocated_ranges[ppa->nr_allocated_ranges++];
	range->addr = addr;
	range->size = size;
}

static void __init ppa_finalize(void)
{
	struct pkeys_physmem_allocator *ppa = &pkeys_physmem_allocator;

	for (unsigned int i = 0; i < ppa->nr_allocated_ranges; i++) {
		struct physmem_range *range = &ppa->allocated_ranges[i];

		set_pkey_pgtable_phys(range->addr, range->size);
	}
}

phys_addr_t __ref kpkeys_physmem_pgtable_alloc(void)
{
	size_t size = PAGE_SIZE;
	phys_addr_t addr;

	addr = memblock_phys_alloc_range(size, size, 0,
					 MEMBLOCK_ALLOC_NOLEAKTRACE);
	if (!addr)
		return addr;

	ppa_register_allocated_range(addr, size);

	return addr;
}
