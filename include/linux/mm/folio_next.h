/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MM_FOLIO_NEXT_H
#define _LINUX_MM_FOLIO_NEXT_H

#include <linux/mm/page_address.h> // for nth_page(), needed by folio_page()
#include <linux/mm/page_size.h> // for folio_nr_pages()

/**
 * folio_next - Move to the next physical folio.
 * @folio: The folio we're currently operating on.
 *
 * If you have physically contiguous memory which may span more than
 * one folio (eg a &struct bio_vec), use this function to move from one
 * folio to the next.  Do not use it if the memory is only virtually
 * contiguous as the folios are almost certainly not adjacent to each
 * other.  This is the folio equivalent to writing ``page++``.
 *
 * Context: We assume that the folios are refcounted and/or locked at a
 * higher level and do not adjust the reference counts.
 * Return: The next struct folio.
 */
static inline struct folio *folio_next(struct folio *folio)
{
	return (struct folio *)folio_page(folio, folio_nr_pages(folio));
}

#endif /* _LINUX_MM_FOLIO_NEXT_H */
