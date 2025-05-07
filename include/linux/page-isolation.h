/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PAGEISOLATION_H
#define __LINUX_PAGEISOLATION_H

#ifdef CONFIG_MEMORY_ISOLATION
static inline bool is_migrate_isolate_page(struct page *page)
{
	return get_pageblock_isolate(page);
}
static inline bool is_migrate_isolate(int migratetype)
{
	return migratetype == MIGRATE_ISOLATE;
}
#else
static inline bool is_migrate_isolate_page(struct page *page)
{
	return false;
}
static inline bool is_migrate_isolate(int migratetype)
{
	return false;
}
#endif

/*
 * Isolation flags:
 * MEMORY_OFFLINE - isolate to offline (!allocate) memory e.g., skip over
 *		    PageHWPoison() pages and PageOffline() pages.
 * REPORT_FAILURE - report details about the failure to isolate the range
 * CMA_ALLOCATION - isolate for CMA allocations
 */
typedef unsigned int __bitwise isol_flags_t;
#define MEMORY_OFFLINE		((__force isol_flags_t)BIT(0))
#define REPORT_FAILURE		((__force isol_flags_t)BIT(1))
#define CMA_ALLOCATION		((__force isol_flags_t)BIT(2))

void set_pageblock_migratetype(struct page *page, int migratetype);

bool pageblock_isolate_and_move_free_pages(struct zone *zone, struct page *page);
bool pageblock_unisolate_and_move_free_pages(struct zone *zone, struct page *page);

int start_isolate_page_range(unsigned long start_pfn, unsigned long end_pfn,
			     isol_flags_t flags);

void undo_isolate_page_range(unsigned long start_pfn, unsigned long end_pfn);

int test_pages_isolated(unsigned long start_pfn, unsigned long end_pfn,
			int isol_flags);
#endif
