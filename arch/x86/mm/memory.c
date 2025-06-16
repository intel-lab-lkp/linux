// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/mm.h>
#include <linux/range.h>
#include <linux/minmax.h>

/*
 * Limit the optimized version of folio_zero_user() to !CONFIG_HIGHMEM.
 * We do that because clear_pages() works on contiguous kernel pages
 * which might not be true under HIGHMEM.
 */
#ifndef CONFIG_HIGHMEM
/*
 * For voluntary preemption models, operate with a max chunk-size of 8MB.
 * (Worst case resched latency of ~1ms, with a clearing BW of ~10GBps.)
 */
#define PAGE_RESCHED_CHUNK	(8 << (20 - PAGE_SHIFT))

static void clear_pages_resched(void *addr, int npages)
{
	int i, remaining;

	if (preempt_model_preemptible()) {
		clear_pages(addr, npages);
		goto out;
	}

	for (i = 0; i < npages/PAGE_RESCHED_CHUNK; i++) {
		clear_pages(addr + i * PAGE_RESCHED_CHUNK * PAGE_SIZE, PAGE_RESCHED_CHUNK);
		cond_resched();
	}

	remaining = npages % PAGE_RESCHED_CHUNK;

	if (remaining)
		clear_pages(addr + i * PAGE_RESCHED_CHUNK * PAGE_SHIFT, remaining);
out:
	cond_resched();
}

/*
 * folio_zero_user() - multi-page clearing.
 *
 * @folio: hugepage folio
 * @addr_hint: faulting address (if any)
 *
 * Overrides common code folio_zero_user(). This version takes advantage of
 * the fact that string instructions in clear_pages() are more performant
 * on larger extents compared to the usual page-at-a-time clearing.
 *
 * Clearing of 2MB pages is split in three parts: pages in the immediate
 * locality of the faulting page, and its left, right regions; with the local
 * neighbourhood cleared last in order to keep cache lines of the target
 * region hot.
 *
 * For GB pages, there is no expectation of cache locality so just do a
 * straight zero.
 *
 * Note that the folio is fully allocated already so we don't do any exception
 * handling.
 */
void folio_zero_user(struct folio *folio, unsigned long addr_hint)
{
	unsigned long base_addr = ALIGN_DOWN(addr_hint, folio_size(folio));
	const long fault_idx = (addr_hint - base_addr) / PAGE_SIZE;
	const struct range pg = DEFINE_RANGE(0, folio_nr_pages(folio) - 1);
	const int width = 2; /* number of pages cleared last on either side */
	struct range r[3];
	int i;

	if (folio_nr_pages(folio) > MAX_ORDER_NR_PAGES) {
		clear_pages_resched(page_address(folio_page(folio, 0)), folio_nr_pages(folio));
		return;
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
		int npages = range_len(&r[i]);

		if (npages > 0)
			clear_pages_resched(page_address(folio_page(folio, r[i].start)), npages);
	}
}
#endif /* CONFIG_HIGHMEM */
