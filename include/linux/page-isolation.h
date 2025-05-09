/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PAGEISOLATION_H
#define __LINUX_PAGEISOLATION_H

#ifdef CONFIG_MEMORY_ISOLATION
static inline bool is_migrate_isolate_page(struct page *page)
{
	return get_pageblock_migratetype(page) == MIGRATE_ISOLATE;
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
 * Isolation modes:
 * ISOLATE_MODE_NONE - isolate for other purposes than those below
 * MEMORY_OFFLINE    - isolate to offline (!allocate) memory e.g., skip over
 *		       PageHWPoison() pages and PageOffline() pages.
 * CMA_ALLOCATION    - isolate for CMA allocations
 */
enum isolate_mode_t {
	ISOLATE_MODE_NONE,
	MEMORY_OFFLINE,
	CMA_ALLOCATION,
};

/*
 * Isolation flags:
 * REPORT_FAILURE - report details about the failure to isolate the range
 */
typedef unsigned int __bitwise isolate_flags_t;
#define REPORT_FAILURE		((__force isolate_flags_t)BIT(0))

void set_pageblock_migratetype(struct page *page, int migratetype);
void set_pageblock_isolate(struct page *page);

bool pageblock_isolate_and_move_free_pages(struct zone *zone, struct page *page);
bool pageblock_unisolate_and_move_free_pages(struct zone *zone, struct page *page);

int start_isolate_page_range(unsigned long start_pfn, unsigned long end_pfn,
			     isolate_mode_t mode, isolate_flags_t flags);

void undo_isolate_page_range(unsigned long start_pfn, unsigned long end_pfn);

int test_pages_isolated(unsigned long start_pfn, unsigned long end_pfn,
			isolate_mode_t mode);
#endif
