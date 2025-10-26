// SPDX-License-Identifier: GPL-2.0
/*
 * GCMA (Guaranteed Contiguous Memory Allocator)
 *
 */

#define pr_fmt(fmt) "gcma: " fmt

#include <linux/cleancache.h>
#include <linux/gcma.h>
#include <linux/hashtable.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include "internal.h"

#define MAX_GCMA_AREAS		64
#define GCMA_AREA_NAME_MAX_LEN	32

struct gcma_area {
	int pool_id;
	unsigned long start_pfn;
	unsigned long end_pfn;
	char name[GCMA_AREA_NAME_MAX_LEN];
};

static struct gcma_area areas[MAX_GCMA_AREAS];
static atomic_t nr_gcma_area = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(gcma_area_lock);

static int free_folio_range(struct gcma_area *area,
			     unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long scanned = 0;
	unsigned long pfn;

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		int err;

		if (!(++scanned % XA_CHECK_SCHED))
			cond_resched();

		err = cleancache_backend_put_folio(area->pool_id, pfn_folio(pfn));
		if (err) {
			pr_warn("PFN %lu: folio is still in use\n", pfn);
			return err;
		}
	}

	return 0;
}

static int alloc_folio_range(struct gcma_area *area,
			      unsigned long start_pfn, unsigned long end_pfn,
			      gfp_t gfp)
{
	unsigned long scanned = 0;
	unsigned long pfn;

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		int err;

		if (!(++scanned % XA_CHECK_SCHED))
			cond_resched();

		err = cleancache_backend_get_folio(area->pool_id, pfn_folio(pfn));
		if (err) {
			free_folio_range(area, start_pfn, pfn);
			return err;
		}
	}

	return 0;
}

static struct gcma_area *find_area(unsigned long start_pfn, unsigned long end_pfn)
{
	int nr_area = atomic_read_acquire(&nr_gcma_area);
	int i;

	for (i = 0; i < nr_area; i++) {
		struct gcma_area *area = &areas[i];

		if (area->end_pfn <= start_pfn)
			continue;

		if (area->start_pfn > end_pfn)
			continue;

		/* The entire range should belong to a single area */
		if (start_pfn < area->start_pfn || end_pfn > area->end_pfn)
			break;

		/* Found the area containing the entire range */
		return area;
	}

	return NULL;
}

int gcma_register_area(const char *name,
		       unsigned long start_pfn, unsigned long count)
{
	LIST_HEAD(folios);
	int i, pool_id;
	int nr_area;
	int ret = 0;

	pool_id = cleancache_backend_register_pool(name);
	if (pool_id < 0)
		return pool_id;

	for (i = 0; i < count; i++) {
		struct folio *folio;

		folio = pfn_folio(start_pfn + i);
		folio_clear_reserved(folio);
		folio_set_count(folio, 0);
		list_add(&folio->lru, &folios);
	}
	folio_zone(pfn_folio(start_pfn))->cma_pages += count;
	cleancache_backend_put_folios(pool_id, &folios);

	spin_lock(&gcma_area_lock);

	nr_area = atomic_read(&nr_gcma_area);
	if (nr_area < MAX_GCMA_AREAS) {
		struct gcma_area *area = &areas[nr_area];

		area->pool_id = pool_id;
		area->start_pfn = start_pfn;
		area->end_pfn = start_pfn + count;
		strscpy(area->name, name);
		/* Ensure above stores complete before we increase the count */
		atomic_set_release(&nr_gcma_area, nr_area + 1);
	} else {
		ret = -ENOMEM;
	}

	spin_unlock(&gcma_area_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(gcma_register_area);

int gcma_alloc_range(unsigned long start_pfn, unsigned long count, gfp_t gfp)
{
	unsigned long end_pfn = start_pfn + count;
	struct gcma_area *area;
	struct folio *folio;
	int err, order = 0;

	gfp = current_gfp_context(gfp);
	if (gfp & __GFP_COMP) {
		if (!is_power_of_2(count))
			return -EINVAL;

		order = ilog2(count);
		if (order >= MAX_PAGE_ORDER)
			return -EINVAL;
	}

	area = find_area(start_pfn, end_pfn);
	if (!area)
		return -EINVAL;

	err = alloc_folio_range(area, start_pfn, end_pfn, gfp);
	if (err)
		return err;

	/*
	 * GCMA returns pages with refcount 1 and expects them to have
	 * the same refcount 1 when they are freed.
	 */
	if (order) {
		folio = pfn_folio(start_pfn);
		post_alloc_hook(&folio->page, order, gfp);
		set_page_refcounted(&folio->page);
		prep_compound_page(&folio->page, order);
	} else {
		for (unsigned long pfn = start_pfn; pfn < end_pfn; pfn++) {
			folio = pfn_folio(pfn);
			post_alloc_hook(&folio->page, order, gfp);
			set_page_refcounted(&folio->page);
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(gcma_alloc_range);

int gcma_free_range(unsigned long start_pfn, unsigned long count)
{
	unsigned long end_pfn = start_pfn + count;
	struct gcma_area *area;
	unsigned long pfn;
	int err = -EINVAL;

	area = find_area(start_pfn, end_pfn);
	if (!area)
		return -EINVAL;

	/* First pass checks and drops folio refcounts */
	for (pfn = start_pfn; pfn < end_pfn;) {
		struct folio *folio = pfn_folio(pfn);
		unsigned long nr_pages = folio_nr_pages(folio);

		if (pfn + nr_pages > end_pfn) {
			end_pfn = pfn;
			goto error;

		}
		if (!folio_ref_dec_and_test(folio)) {
			end_pfn = pfn + nr_pages;
			goto error;
		}
		pfn += nr_pages;
	}

	/* Second pass prepares the folios */
	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		struct folio *folio = pfn_folio(pfn);

		free_pages_prepare(&folio->page, folio_order(folio));
		pfn += folio_nr_pages(folio);
	}

	err = free_folio_range(area, start_pfn, end_pfn);
	if (!err)
		return 0;

error:
	/* Restore folio refcounts */
	for (pfn = start_pfn; pfn < end_pfn;) {
		struct folio *folio = pfn_folio(pfn);

		folio_ref_inc(folio);
		pfn += folio_nr_pages(folio);
	}

	return err;
}
EXPORT_SYMBOL_GPL(gcma_free_range);
