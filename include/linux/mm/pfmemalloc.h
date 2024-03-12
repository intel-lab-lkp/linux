/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MM_PFMEMALLOC_H
#define _LINUX_MM_PFMEMALLOC_H

#include <linux/bits.h> // for BIT()
#include <linux/mm_types.h> // for struct page

/*
 * Return true only if the page has been allocated with
 * ALLOC_NO_WATERMARKS and the low watermark was not
 * met implying that the system is under some pressure.
 */
static inline bool page_is_pfmemalloc(const struct page *page)
{
	/*
	 * lru.next has bit 1 set if the page is allocated from the
	 * pfmemalloc reserves.  Callers may simply overwrite it if
	 * they do not need to preserve that information.
	 */
	return (uintptr_t)page->lru.next & BIT(1);
}

/*
 * Return true only if the folio has been allocated with
 * ALLOC_NO_WATERMARKS and the low watermark was not
 * met implying that the system is under some pressure.
 */
static inline bool folio_is_pfmemalloc(const struct folio *folio)
{
	/*
	 * lru.next has bit 1 set if the page is allocated from the
	 * pfmemalloc reserves.  Callers may simply overwrite it if
	 * they do not need to preserve that information.
	 */
	return (uintptr_t)folio->lru.next & BIT(1);
}

/*
 * Only to be called by the page allocator on a freshly allocated
 * page.
 */
static inline void set_page_pfmemalloc(struct page *page)
{
	page->lru.next = (void *)BIT(1);
}

static inline void clear_page_pfmemalloc(struct page *page)
{
	page->lru.next = NULL;
}

#endif /* _LINUX_MM_PFMEMALLOC_H */
