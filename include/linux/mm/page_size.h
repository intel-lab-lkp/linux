/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MM_PAGE_SIZE_H
#define _LINUX_MM_PAGE_SIZE_H

#include <linux/bitops.h> // for test_bit()
#include <linux/mmdebug.h> // for VM_BUG_ON_PGFLAGS()
#include <linux/mm_types.h> // for struct page
#include <linux/page-flags.h> // for folio_test_large()
#include <asm/page.h> // for PAGE_SIZE, PAGE_SHIFT

/*
 * compound_order() can be called without holding a reference, which means
 * that niceties like page_folio() don't work.  These callers should be
 * prepared to handle wild return values.  For example, PG_head may be
 * set before the order is initialised, or this may be a tail page.
 * See compaction.c for some good examples.
 */
static inline unsigned int compound_order(struct page *page)
{
	struct folio *folio = (struct folio *)page;

	if (!test_bit(PG_head, &folio->flags))
		return 0;
	return folio->_flags_1 & 0xff;
}

/**
 * folio_order - The allocation order of a folio.
 * @folio: The folio.
 *
 * A folio is composed of 2^order pages.  See get_order() for the definition
 * of order.
 *
 * Return: The order of the folio.
 */
static inline unsigned int folio_order(struct folio *folio)
{
	if (!folio_test_large(folio))
		return 0;
	return folio->_flags_1 & 0xff;
}

/* Returns the number of bytes in this potentially compound page. */
static inline unsigned long page_size(struct page *page)
{
	return PAGE_SIZE << compound_order(page);
}

/* Returns the number of bits needed for the number of bytes in a page */
static inline unsigned int page_shift(struct page *page)
{
	return PAGE_SHIFT + compound_order(page);
}

/**
 * thp_order - Order of a transparent huge page.
 * @page: Head page of a transparent huge page.
 */
static inline unsigned int thp_order(struct page *page)
{
	VM_BUG_ON_PGFLAGS(PageTail(page), page);
	return compound_order(page);
}

/**
 * thp_size - Size of a transparent huge page.
 * @page: Head page of a transparent huge page.
 *
 * Return: Number of bytes in this page.
 */
static inline unsigned long thp_size(struct page *page)
{
	return PAGE_SIZE << thp_order(page);
}

/**
 * folio_nr_pages - The number of pages in the folio.
 * @folio: The folio.
 *
 * Return: A positive power of two.
 */
static inline long folio_nr_pages(struct folio *folio)
{
	if (!folio_test_large(folio))
		return 1;
#ifdef CONFIG_64BIT
	return folio->_folio_nr_pages;
#else
	return 1L << (folio->_flags_1 & 0xff);
#endif
}

/*
 * compound_nr() returns the number of pages in this potentially compound
 * page.  compound_nr() can be called on a tail page, and is defined to
 * return 1 in that case.
 */
static inline unsigned long compound_nr(struct page *page)
{
	struct folio *folio = (struct folio *)page;

	if (!test_bit(PG_head, &folio->flags))
		return 1;
#ifdef CONFIG_64BIT
	return folio->_folio_nr_pages;
#else
	return 1L << (folio->_flags_1 & 0xff);
#endif
}

/**
 * thp_nr_pages - The number of regular pages in this huge page.
 * @page: The head page of a huge page.
 */
static inline int thp_nr_pages(struct page *page)
{
	return folio_nr_pages((struct folio *)page);
}

/**
 * folio_shift - The size of the memory described by this folio.
 * @folio: The folio.
 *
 * A folio represents a number of bytes which is a power-of-two in size.
 * This function tells you which power-of-two the folio is.  See also
 * folio_size() and folio_order().
 *
 * Context: The caller should have a reference on the folio to prevent
 * it from being split.  It is not necessary for the folio to be locked.
 * Return: The base-2 logarithm of the size of this folio.
 */
static inline unsigned int folio_shift(struct folio *folio)
{
	return PAGE_SHIFT + folio_order(folio);
}

/**
 * folio_size - The number of bytes in a folio.
 * @folio: The folio.
 *
 * Context: The caller should have a reference on the folio to prevent
 * it from being split.  It is not necessary for the folio to be locked.
 * Return: The number of bytes in this folio.
 */
static inline size_t folio_size(struct folio *folio)
{
	return PAGE_SIZE << folio_order(folio);
}

#endif /* _LINUX_MM_PAGE_SIZE_H */
