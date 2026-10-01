// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Google LLC.
 * Author: Pranjal Shrivastava <praan@google.com>
 * IO Page table reclamation (Shrinker) for generic_pt.
 */

#include <linux/iommu.h>
#include <linux/shrinker.h>
#include <linux/srcu.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include "../iommu-pages.h"
#include <linux/generic_pt/iommu.h>

DEFINE_SRCU(generic_pt_srcu);
EXPORT_SYMBOL_GPL(generic_pt_srcu);

static LIST_HEAD(generic_pt_domains_list);
static DEFINE_MUTEX(generic_pt_domains_list_lock);

void generic_pt_shrinker_add(struct pt_iommu *iommu)
{
	mutex_lock(&generic_pt_domains_list_lock);
	list_add_tail(&iommu->shrinker_list, &generic_pt_domains_list);
	mutex_unlock(&generic_pt_domains_list_lock);
}
EXPORT_SYMBOL_GPL(generic_pt_shrinker_add);

void generic_pt_shrinker_remove(struct pt_iommu *iommu)
{
	mutex_lock(&generic_pt_domains_list_lock);
	list_del(&iommu->shrinker_list);
	mutex_unlock(&generic_pt_domains_list_lock);
}
EXPORT_SYMBOL_GPL(generic_pt_shrinker_remove);

static unsigned long generic_pt_shrinker_count(struct shrinker *shrink,
					       struct shrink_control *sc)
{
	struct pt_iommu *iommu;
	unsigned long count = 0;

	mutex_lock(&generic_pt_domains_list_lock);
	list_for_each_entry(iommu, &generic_pt_domains_list, shrinker_list) {
		struct iommu_domain *domain = &iommu->domain;

		/* TODO: Return real nr_pages here! */
		if (!xa_empty(&domain->reclaim_list))
			count += 1;
	}
	mutex_unlock(&generic_pt_domains_list_lock);

	return count;
}

static unsigned long generic_pt_shrinker_scan(struct shrinker *shrink,
					      struct shrink_control *sc)
{
	struct iommu_pages_list free_list = IOMMU_PAGES_LIST_INIT(free_list);
	struct pt_iommu *iommu;
	struct ioptdesc *ioptdesc;
	unsigned long iova;
	unsigned long freed_count = 0;

	mutex_lock(&generic_pt_domains_list_lock);
	list_for_each_entry(iommu, &generic_pt_domains_list, shrinker_list) {
		struct iommu_domain *domain = &iommu->domain;
		unsigned long prev_freed = freed_count;

		xa_for_each(&domain->reclaim_list, iova, ioptdesc) {
			if (freed_count >= sc->nr_to_scan)
				break;

			if (atomic_cmpxchg(&ioptdesc->__page_refcount, 1, 0) == 1) {
				void *virt = folio_address(ioptdesc_folio(ioptdesc));

				if (iommu->ops->sever_branch(iommu, iova, virt_to_phys(virt))) {
					/* Successfully severed, queue for freeing */
					xa_erase(&domain->reclaim_list, iova);
					iommu_pages_list_add(&free_list, virt);
					freed_count++;
				} else {
					/* We raced with map, abort pruning */
					xa_erase(&domain->reclaim_list, iova);
				}
			} else {
				/* Not empty anymore, remove from list */
				xa_erase(&domain->reclaim_list, iova);
			}
		}

		/*
		 * The IOMMU may still cache pointers to the severed tables in its
		 * paging-structure caches, flush them before the tables are freed.
		 */
		if (freed_count != prev_freed)
			iommu_flush_iotlb_all(domain);
	}
	mutex_unlock(&generic_pt_domains_list_lock);

	/* If we didn't sever anything, just return */
	if (list_empty(&free_list.pages))
		return freed_count;

	/* Wait ONE time for the entire system */
	synchronize_srcu(&generic_pt_srcu);

	/* Free pages from all domains */
	struct page *page, *next;

	list_for_each_entry_safe(page, next, &free_list.pages, lru) {
		/* We must restore the refcount to 1 before freeing */
		set_page_count(page, 1);
	}

	iommu_put_pages_list(&free_list);

	return freed_count;
}

static int __init generic_pt_shrinker_init(void)
{
	struct shrinker *shrinker;

	shrinker = shrinker_alloc(0, "iommu-generic-pt");
	if (!shrinker)
		return -ENOMEM;

	shrinker->count_objects = generic_pt_shrinker_count;
	shrinker->scan_objects = generic_pt_shrinker_scan;

	shrinker_register(shrinker);
	return 0;
}
subsys_initcall(generic_pt_shrinker_init);
