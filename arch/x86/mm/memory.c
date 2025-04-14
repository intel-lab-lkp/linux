// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/mm.h>
#include <linux/range.h>
#include <linux/minmax.h>

#ifndef CONFIG_HIGHMEM
/*
 * folio_zero_user_preemptible(): multi-page clearing variant of folio_zero_user().
 *
 * Taking inspiration from the common code variant, we split the zeroing in
 * three parts: left of the fault, right of the fault, and up to 5 pages
 * in the immediate neighbourhood of the target page.
 *
 * Cleared in that order to keep cache lines of the target region hot.
 *
 * For gigantic pages, there is no expectation of cache locality so just do a
 * straight zero.
 */
void folio_zero_user_preemptible(struct folio *folio, unsigned long addr_hint)
{
	unsigned long base_addr = ALIGN_DOWN(addr_hint, folio_size(folio));
	const long fault_idx = (addr_hint - base_addr) / PAGE_SIZE;
	const struct range pg = DEFINE_RANGE(0, folio_nr_pages(folio) - 1);
	int width = 2; /* pages cleared last on either side */
	struct range r[3];
	int i;

	if (folio_nr_pages(folio) > MAX_ORDER_NR_PAGES) {
		clear_pages(page_address(folio_page(folio, 0)), folio_nr_pages(folio));
		goto out;
	}

	/*
	 * Faulting page and its immediate neighbourhood. Cleared at the end to
	 * ensure it sticks around in the cache.
	 */
	r[2] = DEFINE_RANGE(clamp_t(s64, fault_idx - width, pg.start, pg.end),
			    clamp_t(s64, fault_idx + width, pg.start, pg.end));

	/* Region to the left of the fault */
	r[1] = DEFINE_RANGE(pg.start,
			    clamp_t(s64, r[2].start-1, pg.start-1, r[2].start));

	/* Region to the right of the fault: always valid for the common fault_idx=0 case. */
	r[0] = DEFINE_RANGE(clamp_t(s64, r[2].end+1, r[2].end, pg.end+1),
			    pg.end);

	for (i = 0; i <= 2; i++) {
		int len = range_len(&r[i]);

		if (len > 0)
			clear_pages(page_address(folio_page(folio, r[i].start)), len);
	}

out:
	/* Explicitly invoke cond_resched() to handle any live patching necessary. */
	cond_resched();
}

#endif /* CONFIG_HIGHMEM */
