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

#define MAX_GCMA_AREAS		64
#define GCMA_AREA_NAME_MAX_LEN	32

struct gcma_area {
	int area_id;
	unsigned long start_pfn;
	unsigned long end_pfn;
	char name[GCMA_AREA_NAME_MAX_LEN];
};

static struct gcma_area areas[MAX_GCMA_AREAS];
static atomic_t nr_gcma_area = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(gcma_area_lock);

static void alloc_page_range(struct gcma_area *area,
			     unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long scanned = 0;
	unsigned long pfn;
	struct page *page;
	int err;

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		if (!(++scanned % XA_CHECK_SCHED))
			cond_resched();

		page = pfn_to_page(pfn);
		err = cleancache_backend_get_folio(area->area_id, page_folio(page));
		VM_BUG_ON(err);
	}
}

static void free_page_range(struct gcma_area *area,
			    unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long scanned = 0;
	unsigned long pfn;
	struct page *page;
	int err;

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		if (!(++scanned % XA_CHECK_SCHED))
			cond_resched();

		page = pfn_to_page(pfn);
		err = cleancache_backend_put_folio(area->area_id,
						   page_folio(page));
		VM_BUG_ON(err);
	}
}

int gcma_register_area(const char *name,
		       unsigned long start_pfn, unsigned long count)
{
	LIST_HEAD(folios);
	int i, area_id;
	int nr_area;
	int ret = 0;

	for (i = 0; i < count; i++) {
		struct folio *folio;

		folio = page_folio(pfn_to_page(start_pfn + i));
		list_add(&folio->lru, &folios);
	}

	area_id = cleancache_register_backend(name, &folios);
	if (area_id < 0)
		return area_id;

	spin_lock(&gcma_area_lock);

	nr_area = atomic_read(&nr_gcma_area);
	if (nr_area < MAX_GCMA_AREAS) {
		struct gcma_area *area = &areas[nr_area];

		area->area_id = area_id;
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

void gcma_alloc_range(unsigned long start_pfn, unsigned long count)
{
	int nr_area = atomic_read_acquire(&nr_gcma_area);
	unsigned long end_pfn = start_pfn + count;
	struct gcma_area *area;
	int i;

	for (i = 0; i < nr_area; i++) {
		unsigned long s_pfn, e_pfn;

		area = &areas[i];
		if (area->end_pfn <= start_pfn)
			continue;

		if (area->start_pfn > end_pfn)
			continue;

		s_pfn = max(start_pfn, area->start_pfn);
		e_pfn = min(end_pfn, area->end_pfn);
		alloc_page_range(area, s_pfn, e_pfn);
	}
}
EXPORT_SYMBOL_GPL(gcma_alloc_range);

void gcma_free_range(unsigned long start_pfn, unsigned long count)
{
	int nr_area = atomic_read_acquire(&nr_gcma_area);
	unsigned long end_pfn = start_pfn + count;
	struct gcma_area *area;
	int i;

	for (i = 0; i < nr_area; i++) {
		unsigned long s_pfn, e_pfn;

		area = &areas[i];
		if (area->end_pfn <= start_pfn)
			continue;

		if (area->start_pfn > end_pfn)
			continue;

		s_pfn = max(start_pfn, area->start_pfn);
		e_pfn = min(end_pfn, area->end_pfn);
		free_page_range(area, s_pfn, e_pfn);
	}
}
EXPORT_SYMBOL_GPL(gcma_free_range);
