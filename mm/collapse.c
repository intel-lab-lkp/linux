// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitops.h>
#include <linux/highmem.h>
#include <linux/huge_mm.h>
#include <linux/hugetlb.h>	/* x86 flush_tlb_range() uses hstate_vma() */
#include <linux/leafops.h>
#include <linux/memcontrol.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/pagemap.h>
#include <linux/pgalloc.h>
#include <linux/rmap.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/userfaultfd_k.h>

#include <asm/tlb.h>
#include "collapse.h"
#include "internal.h"

/*
 * Anonymous collapse, in rounds.
 *
 * The folios mapped across a window of PTEs become one folio of that window's
 * order, with the sources quiesced by the two barriers migration uses --
 * migration entries in their PTEs, then a frozen refcount -- so the copy itself
 * needs no lock.  The engine runs under mmap_read throughout.
 *
 * A round carries a batch of candidate windows through the passes together,
 * rather than carrying one window through the whole collapse.  [ptl] and
 * [pmd lock] mark a pass that takes that lock and drops it again, so no
 * page-table lock is ever held across passes; the source folio locks are the
 * exception, held from freeze to putback.  [rcu] marks a pass that takes no
 * page-table lock at all and reads the table racily, which only the scan does.
 *
 * Allocation happens on both sides of the freeze, and which side comes first
 * matters.  collapse_provision() tries first, inside the window and after the
 * freeze, with reclaim masked out of the gfp: the sources are frozen by then,
 * so a faulter on one of them waits for this allocation.  A candidate the
 * allocator has nothing ready for is not failed -- it goes back to selection,
 * and collapse_reserve() allocates for it before the next round freezes
 * anything, outside the window, where reclaim costs khugepaged its own
 * progress and nobody else's wait.
 *
 * That second chance needs the policy's gfp to allow reclaim at all.  When it
 * does not, a retry would miss the same way, so the first miss is the answer.
 *
 * collapse_scan_anon_pmd()            judge one PTE table's worth of a VMA
 * `- collapse_scan_table()            [rcu] a bit per PTE a collapse can use
 *
 * collapse_anon_pmd()                 cut windows from those bits, run them
 * |- collapse_next_candidate()        the next window worth attempting
 * `- collapse_run_batch()             run the batch, then classify it
 *    |- collapse_round()              below
 *    `- collapse_classify_result()    carry on / lower / abandon
 *       `- collapse_push_retry()      queue it for a lower order
 *
 * collapse_round()                    one batch of candidates
 * |- collapse_reserve()               second try for what the last round
 * |                                   missed, with reclaim; sleeps
 * |- collapse_deposit()               a page table per PMD-order candidate
 * |- collapse_revalidate()            check the VMA and the table survived
 * |- collapse_faultin()               make the sources present and exclusive;
 * |                                   sleeps, and may leave the lock dropped
 * |- collapse_freeze()                raise the barriers [ptl], flush the TLB
 * |- collapse_provision()             first try for every other destination,
 * |                                   without reclaim: the sources are frozen
 * |- collapse_copy()                  copy into the destinations; sleeps
 * |- collapse_install()               publish them [ptl], or [pmd lock] and a
 * |                                   second TLB flush at the PMD order
 * |- collapse_putback()               lower the barriers, in order
 * `- collapse_finish()                settle whatever the round reached
 *
 * Every slot of a candidate is a real source, a hole (pte_none, zero-filled
 * and re-verified still-none at install), or the zeropage (cleared at freeze,
 * zero-filled).  Sources come in "spans" -- consecutive PTEs mapping
 * consecutive pages of one folio -- so partially mapped and compound sources
 * collapse too: any order below the window's is a source, and a PMD candidate
 * takes even a PTE-mapped THP of its own order.
 *
 * Nothing calls any of this yet: the anon path still uses the mechanism it
 * replaces, and is switched over once both halves are complete.
 */

/*
 * Scan the PTEs between @start and @end and record what a collapse could use: a
 * bit in cc->eligible_ptes for every PTE that may be a source.  Returns
 * SCAN_SUCCEED when every PTE in the range qualified, otherwise the reason one
 * did not, and narrows cc->select_orders to what is still worth trying here.
 */
static enum scan_result collapse_scan_table(struct vm_area_struct *vma,
					    pmd_t *pmd, unsigned long start,
					    unsigned long end,
					    struct collapse_control *cc)
{
	return SCAN_SUCCEED;
}

/* Everything a table is judged on starts empty for each table */
static void collapse_anon_scan_init(struct collapse_control *cc)
{
	bitmap_zero(cc->eligible_ptes, MAX_PTRS_PER_PTE);
	memset(cc->node_load, 0, sizeof(cc->node_load));
	nodes_clear(cc->alloc_nmask);

	cc->select_orders = 0;
	cc->nr_collapsed = 0;
}

/*
 * Judge one table's worth of @vma, leaving in @cc what a collapse could use:
 * which orders are still worth attempting, and why the table was turned down if
 * some order was.  Holds mmap_lock throughout -- it only reads -- and a caller
 * that acts on what it found hands the range to collapse_anon_pmd() afterwards,
 * without the lock.
 */
static enum scan_result __maybe_unused
collapse_scan_anon_pmd(struct vm_area_struct *vma, unsigned long start,
		       unsigned long end, struct collapse_control *cc)
{
	const unsigned long pmd_addr = start & HPAGE_PMD_MASK;
	struct mm_struct *mm = vma->vm_mm;
	pmd_t *pmd;

	/* One table's worth at most, not empty, and inside the VMA */
	VM_WARN_ON_ONCE(end > pmd_addr + HPAGE_PMD_SIZE || start >= end);
	VM_WARN_ON_ONCE(start < vma->vm_start || end > vma->vm_end);

	cc->scan_refusal = find_pmd_or_thp_or_none(mm, pmd_addr, &pmd);
	if (cc->scan_refusal != SCAN_SUCCEED) {
		cc->progress++;
		return cc->scan_refusal;
	}

	/* Cleared only once a table has turned out to be there */
	collapse_anon_scan_init(cc);

	cc->select_orders = collapse_possible_orders(vma, vma->vm_flags,
						     cc->policy.tva_type);
	if (!cc->select_orders) {
		cc->scan_refusal = SCAN_VMA_CHECK;
		return cc->scan_refusal;
	}

	/* The scan narrows select_orders to whatever is left worth trying */
	cc->scan_refusal = collapse_scan_table(vma, pmd, start, end, cc);

	return cc->scan_refusal;
}

/*
 * Cut the table into candidate windows and collapse what fits, from the
 * largest order downwards.  Returns what the table yielded: a collapse, or
 * the reason it did not.
 */
static enum scan_result __maybe_unused
collapse_anon_pmd(struct mm_struct *mm, unsigned long start, unsigned long end,
		  struct collapse_control *cc)
{
	return SCAN_FAIL;
}
