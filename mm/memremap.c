// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2015 Intel Corporation. All rights reserved. */
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kasan.h>
#include <linux/memory_hotplug.h>
#include <linux/memremap.h>
#include <linux/swap.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/swapops.h>
#include <linux/types.h>
#include <linux/wait_bit.h>
#include <linux/xarray.h>
#include <linux/maple_tree.h>
#include "internal.h"

static DEFINE_XARRAY(pgmap_array);
static struct maple_tree device_private_pgmap_tree =
	MTREE_INIT(device_private_pgmap_tree, MT_FLAGS_ALLOC_RANGE);

/*
 * The memremap() and memremap_pages() interfaces are alternately used
 * to map persistent memory namespaces. These interfaces place different
 * constraints on the alignment and size of the mapping (namespace).
 * memremap() can map individual PAGE_SIZE pages. memremap_pages() can
 * only map subsections (2MB), and at least one architecture (PowerPC)
 * the minimum mapping granularity of memremap_pages() is 16MB.
 *
 * The role of memremap_compat_align() is to communicate the minimum
 * arch supported alignment of a namespace such that it can freely
 * switch modes without violating the arch constraint. Namely, do not
 * allow a namespace to be PAGE_SIZE aligned since that namespace may be
 * reconfigured into a mode that requires SUBSECTION_SIZE alignment.
 */
#ifndef CONFIG_ARCH_HAS_MEMREMAP_COMPAT_ALIGN
unsigned long memremap_compat_align(void)
{
	return SUBSECTION_SIZE;
}
EXPORT_SYMBOL_GPL(memremap_compat_align);
#endif

static void pgmap_array_delete(struct range *range)
{
	xa_store_range(&pgmap_array, PHYS_PFN(range->start), PHYS_PFN(range->end),
			NULL, GFP_KERNEL);
	synchronize_rcu();
}

static unsigned long pfn_first(struct dev_pagemap *pgmap, int range_id)
{
	struct range *range = &pgmap->ranges[range_id];
	unsigned long pfn = PHYS_PFN(range->start);

	if (range_id)
		return pfn;
	return pfn + vmem_altmap_offset(pgmap_altmap(pgmap));
}

bool pgmap_pfn_valid(struct dev_pagemap *pgmap, unsigned long pfn)
{
	int i;

	for (i = 0; i < pgmap->nr_range; i++) {
		struct range *range = &pgmap->ranges[i];

		if (pfn >= PHYS_PFN(range->start) &&
		    pfn <= PHYS_PFN(range->end))
			return pfn >= pfn_first(pgmap, i);
	}

	return false;
}

static unsigned long pfn_end(struct dev_pagemap *pgmap, int range_id)
{
	const struct range *range = &pgmap->ranges[range_id];

	return (range->start + range_len(range)) >> PAGE_SHIFT;
}

static unsigned long pfn_len(struct dev_pagemap *pgmap, unsigned long range_id)
{
	return (pfn_end(pgmap, range_id) -
		pfn_first(pgmap, range_id)) >> pgmap->vmemmap_shift;
}

static void pageunmap_range(struct dev_pagemap *pgmap, int range_id)
{
	struct range *range = &pgmap->ranges[range_id];
	struct page *first_page;

	/* make sure to access a memmap that was actually initialized */
	first_page = pfn_to_page(pfn_first(pgmap, range_id));

	/* pages are dead and unused, undo the arch mapping */
	mem_hotplug_begin();
	remove_pfn_range_from_zone(page_zone(first_page), PHYS_PFN(range->start),
				   PHYS_PFN(range_len(range)));
	if (pgmap->type == MEMORY_DEVICE_PRIVATE) {
		__remove_pages(PHYS_PFN(range->start),
			       PHYS_PFN(range_len(range)), NULL);
	} else {
		arch_remove_memory(range->start, range_len(range),
				pgmap_altmap(pgmap));
		kasan_remove_zero_shadow(__va(range->start), range_len(range));
	}
	mem_hotplug_done();

	pfnmap_untrack(PHYS_PFN(range->start), range_len(range));
	pgmap_array_delete(range);
}

void memunmap_pages(struct dev_pagemap *pgmap)
{
	int i;

	WARN_ONCE(pgmap->type == MEMORY_DEVICE_PRIVATE, "Type should not be MEMORY_DEVICE_PRIVATE\n");

	percpu_ref_kill(&pgmap->ref);
	if (pgmap->type != MEMORY_DEVICE_COHERENT)
		for (i = 0; i < pgmap->nr_range; i++)
			percpu_ref_put_many(&pgmap->ref, pfn_len(pgmap, i));

	wait_for_completion(&pgmap->done);

	for (i = 0; i < pgmap->nr_range; i++)
		pageunmap_range(pgmap, i);
	percpu_ref_exit(&pgmap->ref);

	WARN_ONCE(pgmap->altmap.alloc, "failed to free all reserved pages\n");
}
EXPORT_SYMBOL_GPL(memunmap_pages);

static void devm_memremap_pages_release(void *data)
{
	memunmap_pages(data);
}

static void dev_pagemap_percpu_release(struct percpu_ref *ref)
{
	struct dev_pagemap *pgmap = container_of(ref, struct dev_pagemap, ref);

	complete(&pgmap->done);
}

static int pagemap_range(struct dev_pagemap *pgmap, struct mhp_params *params,
		int range_id, int nid)
{
	struct range *range = &pgmap->ranges[range_id];
	struct dev_pagemap *conflict_pgmap;
	int error, is_ram;

	if (WARN_ONCE(pgmap_altmap(pgmap) && range_id > 0,
				"altmap not supported for multiple ranges\n"))
		return -EINVAL;

	conflict_pgmap = get_dev_pagemap(PHYS_PFN(range->start));
	if (conflict_pgmap) {
		WARN(1, "Conflicting mapping in same section\n");
		put_dev_pagemap(conflict_pgmap);
		return -ENOMEM;
	}

	conflict_pgmap = get_dev_pagemap(PHYS_PFN(range->end));
	if (conflict_pgmap) {
		WARN(1, "Conflicting mapping in same section\n");
		put_dev_pagemap(conflict_pgmap);
		return -ENOMEM;
	}

	is_ram = region_intersects(range->start, range_len(range),
		IORESOURCE_SYSTEM_RAM, IORES_DESC_NONE);

	if (is_ram != REGION_DISJOINT) {
		WARN_ONCE(1, "attempted on %s region %#llx-%#llx\n",
				is_ram == REGION_MIXED ? "mixed" : "ram",
				range->start, range->end);
		return -ENXIO;
	}

	error = xa_err(xa_store_range(&pgmap_array, PHYS_PFN(range->start),
				PHYS_PFN(range->end), pgmap, GFP_KERNEL));
	if (error)
		return error;

	if (nid < 0)
		nid = numa_mem_id();

	error = pfnmap_track(PHYS_PFN(range->start), range_len(range),
			     &params->pgprot);
	if (error)
		goto err_pfn_remap;

	if (!mhp_range_allowed(range->start, range_len(range), true)) {
		error = -EINVAL;
		goto err_kasan;
	}

	mem_hotplug_begin();

	/*
	 * All device memory types except device private memory are accessible
	 * by the CPU, so we want the linear mapping and thus use
	 * arch_add_memory().
	 */
	error = kasan_add_zero_shadow(__va(range->start), range_len(range));
	if (error) {
		mem_hotplug_done();
		goto err_kasan;
	}

	error = arch_add_memory(nid, range->start, range_len(range),
				params);

	if (!error) {
		struct zone *zone;

		zone = &NODE_DATA(nid)->node_zones[ZONE_DEVICE];
		move_pfn_range_to_zone(zone, PHYS_PFN(range->start),
				PHYS_PFN(range_len(range)), params->altmap,
				MIGRATE_MOVABLE, false);
	}

	mem_hotplug_done();
	if (error)
		goto err_add_memory;

	/*
	 * Initialization of the pages has been deferred until now in order
	 * to allow us to do the work while not holding the hotplug lock.
	 */
	memmap_init_zone_device(&NODE_DATA(nid)->node_zones[ZONE_DEVICE],
				PHYS_PFN(range->start),
				PHYS_PFN(range_len(range)), pgmap);
	if (pgmap->type != MEMORY_DEVICE_PRIVATE &&
	    pgmap->type != MEMORY_DEVICE_COHERENT)
		percpu_ref_get_many(&pgmap->ref, pfn_len(pgmap, range_id));
	return 0;

err_add_memory:
	kasan_remove_zero_shadow(__va(range->start), range_len(range));
err_kasan:
	pfnmap_untrack(PHYS_PFN(range->start), range_len(range));
err_pfn_remap:
	pgmap_array_delete(range);
	return error;
}


/*
 * Not device managed version of devm_memremap_pages, undone by
 * memunmap_pages().  Please use devm_memremap_pages if you have a struct
 * device available.
 */
void *memremap_pages(struct dev_pagemap *pgmap, int nid)
{
	struct mhp_params params = {
		.altmap = pgmap_altmap(pgmap),
		.pgmap = pgmap,
		.pgprot = PAGE_KERNEL,
	};
	const int nr_range = pgmap->nr_range;
	int error, i;

	if (WARN_ONCE(!nr_range, "nr_range must be specified\n"))
		return ERR_PTR(-EINVAL);
	if (WARN_ONCE(pgmap->vmemmap_shift > MAX_FOLIO_ORDER,
		      "requested folio size unsupported\n"))
		return ERR_PTR(-EINVAL);

	switch (pgmap->type) {
	case MEMORY_DEVICE_PRIVATE:
		WARN(1, "Use memremap_device_private_pagemap()\n");
		return ERR_PTR(-EINVAL);
		break;
	case MEMORY_DEVICE_COHERENT:
		if (!pgmap->ops->folio_free) {
			WARN(1, "Missing folio_free method\n");
			return ERR_PTR(-EINVAL);
		}
		if (!pgmap->owner) {
			WARN(1, "Missing owner\n");
			return ERR_PTR(-EINVAL);
		}
		break;
	case MEMORY_DEVICE_FS_DAX:
		params.pgprot = pgprot_decrypted(params.pgprot);
		break;
	case MEMORY_DEVICE_GENERIC:
		break;
	case MEMORY_DEVICE_PCI_P2PDMA:
		params.pgprot = pgprot_noncached(params.pgprot);
		break;
	default:
		WARN(1, "Invalid pgmap type %d\n", pgmap->type);
		break;
	}

	init_completion(&pgmap->done);
	error = percpu_ref_init(&pgmap->ref, dev_pagemap_percpu_release, 0,
				GFP_KERNEL);
	if (error)
		return ERR_PTR(error);

	/*
	 * Clear the pgmap nr_range as it will be incremented for each
	 * successfully processed range. This communicates how many
	 * regions to unwind in the abort case.
	 */
	pgmap->nr_range = 0;
	error = 0;
	for (i = 0; i < nr_range; i++) {
		error = pagemap_range(pgmap, &params, i, nid);
		if (error)
			break;
		pgmap->nr_range++;
	}

	if (i < nr_range) {
		memunmap_pages(pgmap);
		pgmap->nr_range = nr_range;
		return ERR_PTR(error);
	}

	return __va(pgmap->ranges[0].start);
}
EXPORT_SYMBOL_GPL(memremap_pages);

/**
 * devm_memremap_pages - remap and provide memmap backing for the given resource
 * @dev: hosting device for @res
 * @pgmap: pointer to a struct dev_pagemap
 *
 * Notes:
 * 1/ At a minimum the range and type members of @pgmap must be initialized
 *    by the caller before passing it to this function
 *
 * 2/ The altmap field may optionally be initialized, in which case
 *    PGMAP_ALTMAP_VALID must be set in pgmap->flags.
 *
 * 3/ The ref field may optionally be provided, in which pgmap->ref must be
 *    'live' on entry and will be killed and reaped at
 *    devm_memremap_pages_release() time, or if this routine fails.
 *
 * 4/ range is expected to be a host memory range that could feasibly be
 *    treated as a "System RAM" range, i.e. not a device mmio range, but
 *    this is not enforced.
 */
void *devm_memremap_pages(struct device *dev, struct dev_pagemap *pgmap)
{
	int error;
	void *ret;

	ret = memremap_pages(pgmap, dev_to_node(dev));
	if (IS_ERR(ret))
		return ret;

	error = devm_add_action_or_reset(dev, devm_memremap_pages_release,
			pgmap);
	if (error)
		return ERR_PTR(error);
	return ret;
}
EXPORT_SYMBOL_GPL(devm_memremap_pages);

void devm_memunmap_pages(struct device *dev, struct dev_pagemap *pgmap)
{
	devm_release_action(dev, devm_memremap_pages_release, pgmap);
}
EXPORT_SYMBOL_GPL(devm_memunmap_pages);

static void devm_memremap_device_private_pagemap_release(void *data)
{
	memunmap_device_private_pagemap(data);
}

int devm_memremap_device_private_pagemap(struct device *dev, struct dev_pagemap *pgmap)
{
	int ret;

	ret = memremap_device_private_pagemap(pgmap, dev_to_node(dev));
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, devm_memremap_device_private_pagemap_release,
			pgmap);
	return ret;
}
EXPORT_SYMBOL_GPL(devm_memremap_device_private_pagemap);

void devm_memunmap_device_private_pagemap(struct device *dev, struct dev_pagemap *pgmap)
{
	devm_release_action(dev, devm_memremap_device_private_pagemap_release, pgmap);
}
EXPORT_SYMBOL_GPL(devm_memunmap_device_private_pagemap);

/**
 * get_dev_pagemap() - take a new live reference on the dev_pagemap for @pfn
 * @pfn: page frame number to lookup page_map
 */
struct dev_pagemap *get_dev_pagemap(unsigned long pfn)
{
	struct dev_pagemap *pgmap;
	resource_size_t phys = PFN_PHYS(pfn);

	rcu_read_lock();
	pgmap = xa_load(&pgmap_array, PHYS_PFN(phys));
	if (pgmap && !percpu_ref_tryget_live_rcu(&pgmap->ref))
		pgmap = NULL;
	rcu_read_unlock();

	return pgmap;
}
EXPORT_SYMBOL_GPL(get_dev_pagemap);

void free_zone_device_folio(struct folio *folio)
{
	struct dev_pagemap *pgmap = folio->pgmap;
	unsigned long nr = folio_nr_pages(folio);
	int i;

	if (WARN_ON_ONCE(!pgmap))
		return;

	mem_cgroup_uncharge(folio);

	if (folio_test_anon(folio)) {
		for (i = 0; i < nr; i++)
			__ClearPageAnonExclusive(folio_page(folio, i));
	} else {
		VM_WARN_ON_ONCE(folio_test_large(folio));
	}

	/*
	 * When a device managed page is freed, the folio->mapping field
	 * may still contain a (stale) mapping value. For example, the
	 * lower bits of folio->mapping may still identify the folio as an
	 * anonymous folio. Ultimately, this entire field is just stale
	 * and wrong, and it will cause errors if not cleared.
	 *
	 * For other types of ZONE_DEVICE pages, migration is either
	 * handled differently or not done at all, so there is no need
	 * to clear folio->mapping.
	 *
	 * FS DAX pages clear the mapping when the folio->share count hits
	 * zero which indicating the page has been removed from the file
	 * system mapping.
	 */
	if (pgmap->type != MEMORY_DEVICE_FS_DAX &&
	    pgmap->type != MEMORY_DEVICE_GENERIC)
		folio->mapping = NULL;

	switch (pgmap->type) {
	case MEMORY_DEVICE_PRIVATE:
	case MEMORY_DEVICE_COHERENT:
		if (WARN_ON_ONCE(!pgmap->ops || !pgmap->ops->folio_free))
			break;
		pgmap->ops->folio_free(folio);
		percpu_ref_put_many(&folio->pgmap->ref, nr);
		break;

	case MEMORY_DEVICE_GENERIC:
		/*
		 * Reset the refcount to 1 to prepare for handing out the page
		 * again.
		 */
		folio_set_count(folio, 1);
		break;

	case MEMORY_DEVICE_FS_DAX:
		wake_up_var(&folio->page);
		break;

	case MEMORY_DEVICE_PCI_P2PDMA:
		if (WARN_ON_ONCE(!pgmap->ops || !pgmap->ops->folio_free))
			break;
		pgmap->ops->folio_free(folio);
		break;
	}
}

void zone_device_page_init(struct page *page, unsigned int order)
{
	VM_WARN_ON_ONCE(order > MAX_ORDER_NR_PAGES);

	/*
	 * Drivers shouldn't be allocating pages after calling
	 * memunmap_pages().
	 */
	WARN_ON_ONCE(!percpu_ref_tryget_many(&page_pgmap(page)->ref, 1 << order));
	set_page_count(page, 1);
	lock_page(page);

	if (order)
		prep_compound_page(page, order);
}
EXPORT_SYMBOL_GPL(zone_device_page_init);

unsigned long memremap_device_private_pagemap(struct dev_pagemap *pgmap, int nid)
{
	unsigned long dpfn, dpfn_first, dpfn_last = 0;
	unsigned long start;
	int rc;

	if (pgmap->type != MEMORY_DEVICE_PRIVATE) {
		WARN(1, "Not device private memory\n");
		return -EINVAL;
	}
	if (!IS_ENABLED(CONFIG_DEVICE_PRIVATE)) {
		WARN(1, "Device private memory not supported\n");
		return -EINVAL;
	}
	if (!pgmap->ops || !pgmap->ops->migrate_to_ram) {
		WARN(1, "Missing migrate_to_ram method\n");
		return -EINVAL;
	}
	if (!pgmap->owner) {
		WARN(1, "Missing owner\n");
		return -EINVAL;
	}

	pgmap->pages = kvzalloc(sizeof(struct page) * pgmap->nr_pages,
			       GFP_KERNEL);
	if (!pgmap->pages)
		return -ENOMEM;

	rc = mtree_alloc_range(&device_private_pgmap_tree, &start, pgmap,
			       pgmap->nr_pages * PAGE_SIZE, 0,
			       1ull << MAX_PHYSMEM_BITS, GFP_KERNEL);
	if (rc < 0)
		goto err_mtree_alloc;

	pgmap->range.start = start;
	pgmap->range.end = pgmap->range.start + (pgmap->nr_pages * PAGE_SIZE) - 1;
	pgmap->nr_range = 1;

	init_completion(&pgmap->done);
	rc = percpu_ref_init(&pgmap->ref, dev_pagemap_percpu_release, 0,
		GFP_KERNEL);
	if (rc < 0)
		goto err_ref_init;

	dpfn_first = pgmap->range.start >> PAGE_SHIFT;
	dpfn_last = dpfn_first + (range_len(&pgmap->range) >> PAGE_SHIFT);
	for (dpfn = dpfn_first; dpfn < dpfn_last; dpfn++) {
		struct page *page = device_private_offset_to_page(dpfn);

		__init_zone_device_page(page, dpfn, ZONE_DEVICE, nid, pgmap);
		page_folio(page)->pgmap = (void *) pgmap;
	}

	return 0;

err_ref_init:
	mtree_erase(&device_private_pgmap_tree, pgmap->range.start);
err_mtree_alloc:
	kvfree(pgmap->pages);
	return rc;
}
EXPORT_SYMBOL_GPL(memremap_device_private_pagemap);

void memunmap_device_private_pagemap(struct dev_pagemap *pgmap)
{
	percpu_ref_kill(&pgmap->ref);
	wait_for_completion(&pgmap->done);
	percpu_ref_exit(&pgmap->ref);
	kvfree(pgmap->pages);
	mtree_erase(&device_private_pgmap_tree, pgmap->range.start);
}
EXPORT_SYMBOL_GPL(memunmap_device_private_pagemap);

struct page *device_private_offset_to_page(unsigned long offset)
{
	struct dev_pagemap *pgmap;

	pgmap = mtree_load(&device_private_pgmap_tree, offset << PAGE_SHIFT);
	if (WARN_ON_ONCE(!pgmap))
		return NULL;

	return &pgmap->pages[offset - (pgmap->range.start >> PAGE_SHIFT)];
}
EXPORT_SYMBOL_GPL(device_private_offset_to_page);

struct page *device_private_entry_to_page(softleaf_t entry)
{
	unsigned long offset;

	if (!((softleaf_is_device_private(entry) ||
	    (softleaf_is_migration_device_private(entry)))))
		return NULL;

	offset = softleaf_to_pfn(entry);
	return device_private_offset_to_page(offset);
}

pgoff_t device_private_page_to_offset(const struct page *page)
{
	struct dev_pagemap *pgmap = (struct dev_pagemap *) page_pgmap(page);

	VM_BUG_ON_PAGE(!is_device_private_page(page), page);

	return (pgmap->range.start >> PAGE_SHIFT) + ((page - pgmap->pages));
}
EXPORT_SYMBOL_GPL(device_private_page_to_offset);
