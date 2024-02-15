/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MM_PAGE_ZONE_H
#define _LINUX_MM_PAGE_ZONE_H

#include <linux/mm_types.h> // for struct page, struct folio
#include <linux/mmzone.h> // for ZONEID_*, NODES_*
#include <linux/page-flags.h> // for PF_POISONED_CHECK()

/*
 * The identification function is mainly used by the buddy allocator for
 * determining if two pages could be buddies. We are not really identifying
 * the zone since we could be using the section number id if we do not have
 * node id available in page flags.
 * We only guarantee that it will return the same value for two combinable
 * pages in a zone.
 */
static inline int page_zone_id(struct page *page)
{
	return (page->flags >> ZONEID_PGSHIFT) & ZONEID_MASK;
}

#ifdef NODE_NOT_IN_PAGE_FLAGS
extern int page_to_nid(const struct page *page);
#else
static inline int page_to_nid(const struct page *page)
{
	struct page *p = (struct page *)page;

	return (PF_POISONED_CHECK(p)->flags >> NODES_PGSHIFT) & NODES_MASK;
}
#endif

static inline int folio_nid(const struct folio *folio)
{
	return page_to_nid(&folio->page);
}

#endif /* _LINUX_MM_PAGE_ZONE_H */
