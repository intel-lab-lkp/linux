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
#include <linux/vmstat.h>

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
 * Cap on the memory a round may hold in flight: destination folios allocated
 * but not yet installed, the fault latency of anything inside a candidate being
 * collapsed, and memcg charge pressure all scale with it.  A dense table is
 * collapsed as several rounds rather than one.
 */
#define COLLAPSE_BATCH_BYTES		SZ_32M

/*
 * How many times a round runs the fault-in pass.  A fault that has to wait drops
 * the lock, and running the pass again costs a walk of the batch but buys at
 * least one completed swap-in; readahead brings a cluster in at a time, so this
 * covers a PMD's default max_ptes_swap.
 */
#define COLLAPSE_FAULTIN_PASSES	8

/* Windows in one table at the finest order collapse cuts */
#define COLLAPSE_TABLE_WINDOWS	(HPAGE_PMD_NR >> COLLAPSE_MIN_MTHP_ORDER)

/*
 * How many candidates a round can hold, fixed by the table geometry: the byte
 * cap decides it at the smallest order collapse builds, but never more than the
 * windows one table has at that order.
 */
#define COLLAPSE_MAX_CANDIDATES						\
	min(COLLAPSE_BATCH_BYTES >> (PAGE_SHIFT + COLLAPSE_MIN_MTHP_ORDER), \
	    COLLAPSE_TABLE_WINDOWS)

/* How far a candidate got, and so what a failure has to undo for it */
enum collapse_candidate_state {
	CAND_SELECTED,		/* collected; nothing held on its behalf yet */
	CAND_SKIPPED,		/* refused; nothing of it left to undo */
};

/*
 * A candidate is an (addr, order) window selected for collapse.  Selection
 * counts in PTE offsets -- the bitmap it reads and the alignment it honours are
 * indexed that way -- while the passes that run a candidate work in addresses,
 * like the page tables and VMAs they touch.  This is where the two meet.
 */
struct collapse_candidate {
	unsigned long addr;
	unsigned int order;
	/* The light allocation missed last round: this one may reclaim for it */
	bool reclaim;
	enum collapse_candidate_state state;
	enum scan_result result;
	struct folio *new_folio;
	pgtable_t deposit;		/* PMD order: fresh table to deposit */
};

static unsigned long candidate_start(const struct collapse_candidate *cand)
{
	return cand->addr;
}

static unsigned long candidate_size(const struct collapse_candidate *cand)
{
	return PAGE_SIZE << cand->order;
}

static unsigned long candidate_end(const struct collapse_candidate *cand)
{
	return candidate_start(cand) + candidate_size(cand);
}

/* Where a candidate sits in the table, in the PTE offsets selection counts in */
static unsigned int candidate_offset(const struct collapse_candidate *cand,
				     unsigned long pmd_addr)
{
	return (cand->addr - pmd_addr) >> PAGE_SHIFT;
}

void collapse_control_release(struct collapse_control *cc)
{
	kfree(cc->candidates);
	cc->candidates = NULL;
}

int collapse_control_init(struct collapse_control *cc)
{
	cc->nr_candidates = 0;
	cc->candidates = kmalloc_objs(*cc->candidates, COLLAPSE_MAX_CANDIDATES);
	if (!cc->candidates)
		return -ENOMEM;
	return 0;
}

/*
 * The scan and the allocation both dropped mmap_lock, so nothing seen before it
 * can be trusted: find the VMA and the PTE table again, and check they still
 * allow every provisioned candidate.
 *
 * This is also where the batch's span is settled, for the invalidate the round
 * issues over it.
 */
static enum scan_result collapse_revalidate(struct vm_area_struct *vma,
					    unsigned long pmd_addr,
					    struct collapse_control *cc,
					    pmd_t **pmdp)
{
	unsigned int i;

	cc->batch_start = ULONG_MAX;
	cc->batch_end = 0;

	for (i = 0; i < cc->nr_candidates; i++) {
		struct collapse_candidate *cand = &cc->candidates[i];

		cc->batch_start = min(cc->batch_start, candidate_start(cand));
		cc->batch_end = max(cc->batch_end, candidate_end(cand));
	}

	return SCAN_SUCCEED;
}

/*
 * Make every source the round needs present and exclusively owned by this mm,
 * by faulting it in as an ordinary access would.  Sleeps, and drops mmap_lock on
 * failure, since a fault may have to be retried with it released.
 *
 * Anything faulted in lands on a per-CPU LRU batch, holding a reference the
 * freeze cannot account for, so the freeze drains those batches before it
 * starts.
 */
static enum scan_result collapse_faultin(struct vm_area_struct *vma,
					 struct collapse_control *cc,
					 pmd_t *pmd)
{
	return SCAN_SUCCEED;
}

/*
 * Raise the two barriers on the sources of every candidate: migration entries in
 * their PTEs, then a frozen refcount.  Takes the table's ptl once for the whole
 * batch, and flushes the TLB once before dropping it.  A candidate whose sources
 * moved is dropped here.
 */
static void collapse_freeze(struct vm_area_struct *vma,
			    struct collapse_control *cc, pmd_t *pmd)
{
}

/*
 * Allocate one candidate's destination with @gfp: a folio of its order, charged,
 * with the memcg's deferred-split list heads in place so the install cannot need
 * to allocate under the pmd lock.  Those heads cost only the first collapse in a
 * memcg.
 *
 * A failure counts nothing and changes nothing: what a miss means is the caller's
 * policy.
 */
static enum scan_result collapse_alloc(struct mm_struct *mm,
				       struct collapse_control *cc,
				       struct collapse_candidate *cand,
				       gfp_t gfp)
{
	struct folio *folio;

	folio = __folio_alloc(gfp, cand->order, collapse_find_target_node(cc),
			      &cc->alloc_nmask);
	if (!folio)
		return SCAN_ALLOC_HUGE_PAGE_FAIL;

	if (unlikely(mem_cgroup_charge(folio, mm, gfp)) ||
	    folio_memcg_alloc_deferred(folio)) {
		folio_put(folio);
		return SCAN_CGROUP_CHARGE_FAIL;
	}

	if (is_pmd_order(cand->order)) {
		count_vm_event(THP_COLLAPSE_ALLOC);
		count_memcg_folio_events(folio, THP_COLLAPSE_ALLOC, 1);
	}
	count_mthp_stat(cand->order, MTHP_STAT_COLLAPSE_ALLOC);
	cand->new_folio = folio;

	return SCAN_SUCCEED;
}

/*
 * Allocate ahead of the freeze for the candidates whose light allocation missed
 * last round.  This is where reclaim belongs: nothing is held or frozen, so a
 * long compaction costs only khugepaged's own progress -- which is why the
 * mechanism this replaces allocated here too.  Having asked the allocator to try
 * hard, a miss now is a failure.
 */
static void collapse_reserve(struct mm_struct *mm, struct collapse_control *cc)
{
	unsigned int i;

	for (i = 0; i < cc->nr_candidates; i++) {
		struct collapse_candidate *cand = &cc->candidates[i];
		enum scan_result result;

		if (!cand->reclaim)
			continue;
		cand->reclaim = false;

		result = collapse_alloc(mm, cc, cand, cc->policy.gfp);
		if (result == SCAN_SUCCEED)
			continue;

		if (result == SCAN_ALLOC_HUGE_PAGE_FAIL) {
			/* Asked the allocator to try hard and it still missed */
			if (is_pmd_order(cand->order))
				count_vm_event(THP_COLLAPSE_ALLOC_FAILED);
			count_mthp_stat(cand->order,
					MTHP_STAT_COLLAPSE_ALLOC_FAILED);
		}

		cand->state = CAND_SKIPPED;
		cand->result = result;
	}
}

/*
 * Secure the page table the PMD terminal layer deposits.  This stays ahead of the
 * freeze because pte_alloc_one() allocates with GFP_PGTABLE_USER and takes no gfp
 * to strip: order-0 or not, it may reclaim and sleep, which is what the window
 * exists to keep out.  The destination folio has a light gfp to fall back on and
 * so can be deferred; this has none.
 *
 * A round is one table and a PMD-order window is the whole of it, so such a
 * candidate cannot share a round: if there is one it is the only one, and it is
 * candidates[0].  This secures one page table, never a batch of them.
 */
static void collapse_deposit(struct mm_struct *mm, struct collapse_control *cc)
{
	struct collapse_candidate *cand = &cc->candidates[0];

	if (!is_pmd_order(cand->order))
		return;

	VM_WARN_ON_ONCE(cc->nr_candidates != 1);

	if (cand->state != CAND_SELECTED)
		return;

	cand->deposit = pte_alloc_one(mm);
	if (!cand->deposit) {
		cand->state = CAND_SKIPPED;
		cand->result = SCAN_ALLOC_HUGE_PAGE_FAIL;
	}
}

/*
 * Give the frozen candidates that still need one a destination folio, without
 * reclaim: a faulter on their sources would wait for it.
 *
 * A miss here is not a failure, as long as a retry could do better: the
 * candidate keeps its freeze and asks for the reclaiming gfp, which
 * collapse_reserve() uses before the next round freezes anything.  When the
 * policy forbids reclaim there is nothing better to retry with, so the miss is
 * the answer, and a smaller order over the same region is the better next move.
 */
static void collapse_provision(struct mm_struct *mm,
			       struct collapse_control *cc)
{
	const gfp_t gfp = cc->policy.gfp & ~__GFP_DIRECT_RECLAIM;
	const bool may_retry = gfp != cc->policy.gfp;
	unsigned int i;

	for (i = 0; i < cc->nr_candidates; i++) {
		struct collapse_candidate *cand = &cc->candidates[i];
		enum scan_result result;

		if (cand->state != CAND_SELECTED || cand->new_folio)
			continue;

		result = collapse_alloc(mm, cc, cand, gfp);
		if (result == SCAN_SUCCEED)
			continue;

		if (may_retry) {
			/* A charge miss too: charging may reclaim when allowed */
			cand->result = SCAN_ALLOC_LIGHT_MISS;
		} else {
			/* The gfp a retry would use, so this is the answer */
			if (result == SCAN_ALLOC_HUGE_PAGE_FAIL) {
				if (is_pmd_order(cand->order))
					count_vm_event(THP_COLLAPSE_ALLOC_FAILED);
				count_mthp_stat(cand->order,
						MTHP_STAT_COLLAPSE_ALLOC_FAILED);
			}
			cand->result = result;
		}
	}
}

/*
 * Copy the frozen sources into their destinations.  Nothing can reach either
 * side, so this needs no page-table lock, and it sleeps.
 */
static void collapse_copy(struct vm_area_struct *vma,
			  struct collapse_control *cc)
{
}

/* Publish each destination folio in place of the sources it replaces */
static void collapse_install(struct vm_area_struct *vma,
			     struct collapse_control *cc, pmd_t *pmd)
{
}

/*
 * Lower the barriers the freeze raised, on the sources of an installed candidate
 * and on those of one that got no further.
 */
static void collapse_putback(struct vm_area_struct *vma,
			     struct collapse_control *cc)
{
}

/*
 * Settle whatever the round reached: account what was installed, release what
 * was not, and give every candidate the result selection will classify.  Returns
 * how many candidates were installed.
 */
static unsigned int collapse_finish(struct mm_struct *mm,
				    struct collapse_control *cc,
				    enum scan_result result)
{
	return 0;
}

/*
 * Carry one batch of candidates through the passes.  Every candidate comes back
 * with a result of its own: the passes before the freeze mark what they refuse
 * and carry on, each pass after it works on what the last left, so no failure
 * truncates the round.
 */
static void collapse_round(struct mm_struct *mm, unsigned long pmd_addr,
			   struct collapse_control *cc)
{
	unsigned int passes = COLLAPSE_FAULTIN_PASSES;
	struct mmu_notifier_range range;
	struct vm_area_struct *vma;
	enum scan_result result;
	pmd_t *pmd;

	collapse_reserve(mm, cc);
	collapse_deposit(mm, cc);

retry:
	mmap_read_lock(mm);

	vma = find_vma(mm, pmd_addr);
	if (!vma) {
		result = SCAN_VMA_NULL;
		goto out_unlock;
	}

	result = collapse_revalidate(vma, pmd_addr, cc, &pmd);
	if (result != SCAN_SUCCEED)
		goto out_unlock;

	result = collapse_faultin(vma, cc, pmd);
	/*
	 * A fault dropped the lock to wait, as a swap-in does.  The swap-in it
	 * started is still running and the walk skips whatever has arrived, so
	 * take the lock again rather than send the batch back to selection.  The
	 * VMA and the table are looked up afresh: both may have changed.
	 */
	if (result == SCAN_LOCK_DROPPED && --passes)
		goto retry;
	if (result != SCAN_SUCCEED)
		goto out;	/* the callee released mmap_lock */

	/* One invalidate window spans the batch, as collapse_revalidate() left it */
	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, mm,
				cc->batch_start, cc->batch_end);
	mmu_notifier_invalidate_range_start(&range);

	/*
	 * None of these can fail as a whole: the freeze takes the sources it
	 * can and drops the candidates it cannot, and each pass after it works
	 * on what the one before left, so every barrier raised is lowered again.
	 */
	collapse_freeze(vma, cc, pmd);
	collapse_provision(mm, cc);
	collapse_copy(vma, cc);
	collapse_install(vma, cc, pmd);
	collapse_putback(vma, cc);

	mmu_notifier_invalidate_range_end(&range);

out_unlock:
	mmap_read_unlock(mm);
out:
	collapse_finish(mm, cc, result);
}

/*
 * Is @count past a limit stated per PMD, when only part of a table was scanned?
 * Scale the comparison to the table so a partial scan is held to the same
 * density as a whole one.
 */
static bool collapse_exceeds_limit(unsigned int count, unsigned int max_per_pmd,
				   unsigned long start, unsigned long end)
{
	const unsigned long nr_scanned = (end - start) >> PAGE_SHIFT;

	return (unsigned long)count * HPAGE_PMD_NR >
	       (unsigned long)max_per_pmd * nr_scanned;
}

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
	const unsigned long pmd_addr = start & HPAGE_PMD_MASK;
	unsigned int max_ptes_none, max_ptes_swap, max_ptes_shared;
	int none_or_zero = 0, shared = 0, referenced = 0, unmapped = 0;
	enum scan_result result, pmd_result = SCAN_SUCCEED;
	unsigned int first_offset;
	unsigned long addr;
	pte_t *pte;
	int i;

	max_ptes_none = collapse_max_ptes_none(cc, vma, HPAGE_PMD_ORDER);
	max_ptes_swap = collapse_max_ptes_swap(cc, HPAGE_PMD_ORDER);
	max_ptes_shared = collapse_max_ptes_shared(cc, HPAGE_PMD_ORDER);

	/*
	 * No page table lock: what this builds is advice, and the freeze settles
	 * every question it asks by re-reading the table under the lock and
	 * freezing each source to the count it expects.  A racy read can only
	 * cost a candidate that the freeze then refuses, or miss one that the
	 * next pass finds.  What it buys is that a fault in this range does not
	 * wait for a scan of the whole table.
	 *
	 * pte_offset_map() holds rcu_read_lock() until pte_unmap(), which is
	 * what keeps the table itself from being freed underneath the walk;
	 * mmap_lock keeps the VMA attached, without which free_pgtables() could
	 * free it without waiting for RCU at all.  Nothing below here sleeps.
	 */
	pte = pte_offset_map(pmd, start);
	if (!pte) {
		cc->progress++;
		result = SCAN_NO_PTE_TABLE;
		goto out_no_table;
	}

	/*
	 * The bitmap and the selection offsets stay relative to the table:
	 * natural-alignment math needs the table-absolute position, not the
	 * position within an arbitrarily placed VMA.
	 */
	first_offset = (start - pmd_addr) >> PAGE_SHIFT;
	for (i = first_offset, addr = start; addr < end;
	     i++, addr += PAGE_SIZE) {
		pte_t pteval = ptep_get(pte + (i - first_offset));
		struct folio *folio;
		struct page *page;
		int node;

		cc->progress++;

		if (pte_none_or_zero(pteval)) {
			if (++none_or_zero > max_ptes_none &&
			    pmd_result == SCAN_SUCCEED) {
				pmd_result = SCAN_EXCEED_NONE_PTE;
				count_vm_event(THP_SCAN_EXCEED_NONE_PTE);
				count_mthp_stat(HPAGE_PMD_ORDER,
						MTHP_STAT_COLLAPSE_EXCEED_NONE);
			}
			continue;
		}
		if (!pte_present(pteval)) {
			unmapped++;
			if (collapse_exceeds_limit(unmapped, max_ptes_swap,
						   start, end)) {
				result = SCAN_EXCEED_SWAP_PTE;
				count_vm_event(THP_SCAN_EXCEED_SWAP_PTE);
				count_mthp_stat(HPAGE_PMD_ORDER,
						MTHP_STAT_COLLAPSE_EXCEED_SWAP);
				goto out_table_refused;
			}
			/* Swap entries armed with uffd-wp are refused too */
			if (pte_swp_uffd_any(pteval) &&
			    pmd_result == SCAN_SUCCEED)
				pmd_result = SCAN_PTE_UFFD;
			continue;
		}
		if (pte_uffd(pteval)) {
			/*
			 * The huge PMD could be marked write protected when any
			 * of the small ones is, but that could deliver
			 * userfaults outside the registered range.  Keep it
			 * simple and refuse the PTE.
			 */
			if (pmd_result == SCAN_SUCCEED)
				pmd_result = SCAN_PTE_UFFD;
			continue;
		}

		page = vm_normal_page(vma, addr, pteval);
		if (unlikely(!page) || unlikely(is_zone_device_page(page))) {
			if (pmd_result == SCAN_SUCCEED)
				pmd_result = SCAN_PAGE_NULL;
			continue;
		}
		folio = page_folio(page);

		/*
		 * A VM_DROPPABLE VMA keeps the lazyfree property across the
		 * collapse, so there is nothing to preserve by skipping.
		 */
		if (cc->policy.skip_lazyfree &&
		    !(vma->vm_flags & VM_DROPPABLE) &&
		    folio_test_lazyfree(folio) && !pte_dirty(pteval)) {
			if (pmd_result == SCAN_SUCCEED)
				pmd_result = SCAN_PAGE_LAZYFREE;
			continue;
		}

		if (!folio_test_anon(folio)) {
			if (pmd_result == SCAN_SUCCEED)
				pmd_result = SCAN_PAGE_ANON;
			continue;
		}

		/*
		 * A page counts as shared if any part of its folio is, which
		 * bounds the cost of CoW-breaking rather than the count of it:
		 * collapse_faultin() unshares on !PageAnonExclusive(), a broader
		 * test -- a page whose fork co-mapper has exited is
		 * single-mapped, so not counted here, yet stays non-exclusive
		 * until a write reuses it.  Those are the cheap ones, reused in
		 * place.  A page that has to be copied is one this test catches,
		 * so the limit does bound the copying it is there to bound.
		 */
		if (folio_maybe_mapped_shared(folio)) {
			shared++;
			if (collapse_exceeds_limit(shared, max_ptes_shared,
						   start, end)) {
				result = SCAN_EXCEED_SHARED_PTE;
				count_vm_event(THP_SCAN_EXCEED_SHARED_PTE);
				count_mthp_stat(HPAGE_PMD_ORDER,
						MTHP_STAT_COLLAPSE_EXCEED_SHARED);
				goto out_table_refused;
			}
		}

		/*
		 * Which node the sources are on decides where the destination is
		 * allocated: the one with the most of them wins.
		 */
		node = folio_nid(folio);
		if (collapse_scan_abort(node, cc)) {
			result = SCAN_SCAN_ABORT;
			goto out_table_refused;
		}
		cc->node_load[node]++;

		/*
		 * Usually a folio somebody else is already isolating, whose
		 * reference the freeze would refuse anyway.  Not exact: one
		 * still on a per-CPU add batch reads the same, and the freeze
		 * drains those before it starts.
		 */
		if (!folio_test_lru(folio)) {
			if (pmd_result == SCAN_SUCCEED)
				pmd_result = SCAN_PAGE_LRU;
			continue;
		}
		if (folio_test_locked(folio)) {
			if (pmd_result == SCAN_SUCCEED)
				pmd_result = SCAN_PAGE_LOCK;
			continue;
		}

		/*
		 * A folio whose reference count its mappings do not account for
		 * -- a GUP pin, say -- is refused by the freeze, not here.
		 * folio_expected_ref_count() wants a folio that cannot change
		 * order while it is read, and this walk holds no page table lock
		 * and no folio lock, so a folio splitting underneath it would
		 * have the count read for the wrong size.  A reference of our
		 * own would not help: it stops the folio being freed, not split.
		 *
		 * So leave it to the freeze, which reads the table under the
		 * lock and settles the question by freezing each source to the
		 * count it expects.  What it costs is a window selected here and
		 * refused there.
		 */

		/*
		 * Every check passed: this PTE can be a collapse source.  The
		 * bit is set last, so a disqualified PTE leaves it clear.
		 */
		__set_bit(i, cc->eligible_ptes);

		/*
		 * Whether a range has to look used at all is the caller's
		 * policy, so only a caller that asks gathers the evidence.
		 */
		if (cc->policy.require_referenced &&
		    (pte_young(pteval) || folio_test_young(folio) ||
		     folio_test_referenced(folio) ||
		     mmu_notifier_test_young(vma->vm_mm, addr)))
			referenced++;
	}

	if (cc->policy.require_referenced &&
	    (!referenced || (unmapped && referenced < HPAGE_PMD_NR / 2)))
		result = SCAN_LACK_REFERENCED_PAGE;
	else
		result = pmd_result;
	pte_unmap(pte);
	goto out;

out_table_refused:
	/*
	 * The table is refused as a unit -- a limit the whole range exceeds, or
	 * pages on nodes too distant for one folio to serve them all -- so no
	 * window inside it is eligible either.
	 */
	pte_unmap(pte);
out_no_table:
	cc->select_orders = 0;
out:
	/*
	 * A PMD candidate needs the whole table, so anything that disqualified a
	 * single PTE rules it out.  Smaller windows that avoid the offending
	 * PTEs are still collapsible, so drop just that order and leave the rest
	 * to selection -- dropping it also lowers the order selection roots its
	 * windows at.  MADV_COLLAPSE has no other order enabled, so it is left
	 * with none.
	 */
	if (result != SCAN_SUCCEED)
		cc->select_orders &= ~BIT(HPAGE_PMD_ORDER);

	return result;
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

/* Point the selection cursor at [start, end) of the table, in PTE offsets */
static void collapse_selection_init(struct collapse_control *cc,
				    unsigned int start, unsigned int end)
{
}

/*
 * The next window worth attempting, as an (offset, order) pair.  False when
 * selection is exhausted, which is what ends the range.
 *
 * A candidate is only ever an (offset, order) pair: the scan that recorded the
 * eligible PTEs has dropped the ptl, so anything else -- folio pointers in
 * particular -- would be stale by construction.
 */
static bool collapse_next_candidate(struct collapse_control *cc,
				    unsigned int *offset, unsigned int *order)
{
	return false;
}

/*
 * Feed one candidate's outcome back into selection: its region is done, it
 * re-enters the retry store at a lower order, or the table is abandoned.
 * Returns false in that last case.
 */
static bool collapse_classify_result(struct collapse_control *cc,
				     unsigned int offset, unsigned int order,
				     enum scan_result result)
{
	return true;
}

/*
 * Run and classify the collected batch.  Returns false when a candidate's
 * outcome abandons the table.
 */
static bool collapse_run_batch(struct mm_struct *mm, unsigned long pmd_addr,
			       struct collapse_control *cc)
{
	unsigned int i;

	/* collapse_anon_pmd() only runs a round it has put something in */
	VM_WARN_ON_ONCE(!cc->nr_candidates);

	collapse_round(mm, pmd_addr, cc);

	for (i = 0; i < cc->nr_candidates; i++) {
		struct collapse_candidate *cand = &cc->candidates[i];
		unsigned int offset = candidate_offset(cand, pmd_addr);

		if (!collapse_classify_result(cc, offset, cand->order,
					      cand->result)) {
			/*
			 * The table is abandoned: the candidates behind this one
			 * keep their results and are left unclassified, so
			 * nothing more of this table enters selection, and the
			 * abandoning result clears what earlier ones left there.
			 */
			cc->nr_candidates = 0;
			return false;
		}
	}

	cc->nr_candidates = 0;
	return true;
}

/*
 * One more candidate of @order would either overflow the array or push what the
 * round holds past the byte cap.  An empty round takes whatever it is offered:
 * a single candidate is above the cap all by itself once a PMD is (512M with
 * 64K pages), and refusing it would collapse nothing at all.
 */
static bool collapse_batch_full(struct collapse_control *cc,
				unsigned long bytes, unsigned int order)
{
	if (!cc->nr_candidates)
		return false;

	return cc->nr_candidates == COLLAPSE_MAX_CANDIDATES ||
	       bytes + (PAGE_SIZE << order) > COLLAPSE_BATCH_BYTES;
}

/*
 * Take the next array slot for the window at @addr.  A slot may still hold a
 * previous round's values, so every field is set here.
 */
static void collapse_add_candidate(struct collapse_control *cc,
				   unsigned long addr, unsigned int order)
{
	struct collapse_candidate *cand;

	/* collapse_batch_full() has already made room */
	if (WARN_ON_ONCE(cc->nr_candidates >= COLLAPSE_MAX_CANDIDATES))
		return;

	cand = &cc->candidates[cc->nr_candidates];
	cc->nr_candidates++;
	cand->addr = addr;
	cand->order = order;
	cand->reclaim = false;
	cand->state = CAND_SELECTED;
	cand->result = SCAN_FAIL;
	cand->new_folio = NULL;
	cand->deposit = NULL;
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
	const unsigned long pmd_addr = start & HPAGE_PMD_MASK;
	unsigned int offset, order;
	unsigned long bytes = 0;
	bool pending = false;
	bool cont = true;

	collapse_selection_init(cc, (start - pmd_addr) >> PAGE_SHIFT,
				(end - pmd_addr) >> PAGE_SHIFT);

	while (cont) {
		if (!pending)
			pending = collapse_next_candidate(cc, &offset, &order);

		if (!pending || collapse_batch_full(cc, bytes, order)) {
			/*
			 * Selection is exhausted and the round is empty: the
			 * range is done.  Without this a flush of an empty
			 * round would return, collect nothing, and come
			 * straight back here.
			 */
			if (!cc->nr_candidates)
				break;

			cont = collapse_run_batch(mm, pmd_addr, cc);
			bytes = 0;
			continue;
		}

		/*
		 * The round holds no resources until it is run, so
		 * collecting costs nothing but the array slot.  A candidate the
		 * full round could not take is kept pending for the next one.
		 */
		collapse_add_candidate(cc, pmd_addr + offset * PAGE_SIZE, order);

		bytes += PAGE_SIZE << order;
		pending = false;
	}

	return cc->nr_collapsed ? SCAN_SUCCEED : SCAN_FAIL;
}
