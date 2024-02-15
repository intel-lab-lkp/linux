/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MM_PAGE_USAGE_H
#define _LINUX_MM_PAGE_USAGE_H

#include <linux/mm/devmap_managed.h> // for put_devmap_managed_page()
#include <linux/mmdebug.h> // for VM_BUG_ON_PAGE()
#include <linux/mm_types.h> // for struct folio
#include <linux/page_ref.h>

/*
 * Methods to modify the page usage count.
 *
 * What counts for a page usage:
 * - cache mapping   (page->mapping)
 * - private data    (page->private)
 * - page mapped in a task's page tables, each mapping
 *   is counted separately
 *
 * Also, many kernel routines increase the page count before a critical
 * routine so they can be sure the page doesn't go away from under them.
 */

/*
 * Drop a ref, return true if the refcount fell to zero (the page has no users)
 */
static inline int put_page_testzero(struct page *page)
{
	VM_BUG_ON_PAGE(page_ref_count(page) == 0, page);
	return page_ref_dec_and_test(page);
}

static inline int folio_put_testzero(struct folio *folio)
{
	return put_page_testzero(&folio->page);
}

/*
 * Try to grab a ref unless the page has a refcount of zero, return false if
 * that is the case.
 * This can be called when MMU is off so it must not access
 * any of the virtual mappings.
 */
static inline bool get_page_unless_zero(struct page *page)
{
	return page_ref_add_unless(page, 1, 0);
}

static inline struct folio *folio_get_nontail_page(struct page *page)
{
	if (unlikely(!get_page_unless_zero(page)))
		return NULL;
	return (struct folio *)page;
}

void __folio_put(struct folio *folio);

/* 127: arbitrary random number, small enough to assemble well */
#define folio_ref_zero_or_close_to_overflow(folio) \
	((unsigned int) folio_ref_count(folio) + 127u <= 127u)

/**
 * folio_get - Increment the reference count on a folio.
 * @folio: The folio.
 *
 * Context: May be called in any context, as long as you know that
 * you have a refcount on the folio.  If you do not already have one,
 * folio_try_get() may be the right interface for you to use.
 */
static inline void folio_get(struct folio *folio)
{
	VM_BUG_ON_FOLIO(folio_ref_zero_or_close_to_overflow(folio), folio);
	folio_ref_inc(folio);
}

static inline void get_page(struct page *page)
{
	folio_get(page_folio(page));
}

static inline __must_check bool try_get_page(struct page *page)
{
	page = compound_head(page);
	if (WARN_ON_ONCE(page_ref_count(page) <= 0))
		return false;
	page_ref_inc(page);
	return true;
}

/**
 * folio_put - Decrement the reference count on a folio.
 * @folio: The folio.
 *
 * If the folio's reference count reaches zero, the memory will be
 * released back to the page allocator and may be used by another
 * allocation immediately.  Do not access the memory or the struct folio
 * after calling folio_put() unless you can be sure that it wasn't the
 * last reference.
 *
 * Context: May be called in process or interrupt context, but not in NMI
 * context.  May be called while holding a spinlock.
 */
static inline void folio_put(struct folio *folio)
{
	if (folio_put_testzero(folio))
		__folio_put(folio);
}

/**
 * folio_put_refs - Reduce the reference count on a folio.
 * @folio: The folio.
 * @refs: The amount to subtract from the folio's reference count.
 *
 * If the folio's reference count reaches zero, the memory will be
 * released back to the page allocator and may be used by another
 * allocation immediately.  Do not access the memory or the struct folio
 * after calling folio_put_refs() unless you can be sure that these weren't
 * the last references.
 *
 * Context: May be called in process or interrupt context, but not in NMI
 * context.  May be called while holding a spinlock.
 */
static inline void folio_put_refs(struct folio *folio, int refs)
{
	if (folio_ref_sub_and_test(folio, refs))
		__folio_put(folio);
}

/*
 * union release_pages_arg - an array of pages or folios
 *
 * release_pages() releases a simple array of multiple pages, and
 * accepts various different forms of said page array: either
 * a regular old boring array of pages, an array of folios, or
 * an array of encoded page pointers.
 *
 * The transparent union syntax for this kind of "any of these
 * argument types" is all kinds of ugly, so look away.
 */
typedef union {
	struct page **pages;
	struct folio **folios;
	struct encoded_page **encoded_pages;
} release_pages_arg __attribute__ ((__transparent_union__));

void release_pages(release_pages_arg, int nr);

/**
 * folios_put - Decrement the reference count on an array of folios.
 * @folios: The folios.
 * @nr: How many folios there are.
 *
 * Like folio_put(), but for an array of folios.  This is more efficient
 * than writing the loop yourself as it will optimise the locks which
 * need to be taken if the folios are freed.
 *
 * Context: May be called in process or interrupt context, but not in NMI
 * context.  May be called while holding a spinlock.
 */
static inline void folios_put(struct folio **folios, unsigned int nr)
{
	release_pages(folios, nr);
}

static inline void put_page(struct page *page)
{
	struct folio *folio = page_folio(page);

	/*
	 * For some devmap managed pages we need to catch refcount transition
	 * from 2 to 1:
	 */
	if (put_devmap_managed_page(&folio->page))
		return;
	folio_put(folio);
}

#endif /* _LINUX_MM_PAGE_USAGE_H */
