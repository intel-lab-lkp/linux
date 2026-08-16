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
#include <trace/events/huge_memory.h>
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

static inline enum scan_result check_pmd_state(pmd_t *pmd)
{
	pmd_t pmde = pmdp_get_lockless(pmd);

	if (pmd_none(pmde))
		return SCAN_NO_PTE_TABLE;

	/*
	 * The folio may be under migration when khugepaged is trying to
	 * collapse it. Migration success or failure will eventually end
	 * up with a present PMD mapping a folio again.
	 */
	if (pmd_is_migration_entry(pmde))
		return SCAN_PMD_MAPPED;
	if (!pmd_present(pmde))
		return SCAN_NO_PTE_TABLE;
	if (pmd_trans_huge(pmde))
		return SCAN_PMD_MAPPED;
	if (pmd_bad(pmde))
		return SCAN_NO_PTE_TABLE;
	return SCAN_SUCCEED;
}

enum scan_result find_pmd_or_thp_or_none(struct mm_struct *mm,
		unsigned long address, pmd_t **pmd)
{
	*pmd = mm_find_pmd(mm, address);
	if (!*pmd)
		return SCAN_NO_PTE_TABLE;

	return check_pmd_state(*pmd);
}

/*
 * Check what orders are possible based on the vma and collapse type.
 * This is used to determine if mTHP collapse is a viable option.
 */
unsigned long collapse_possible_orders(struct vm_area_struct *vma,
		vm_flags_t vm_flags, enum tva_type tva_flags)
{
	unsigned long orders;

	/* If khugepaged is scanning an anonymous vma, allow mTHP collapse */
	if ((tva_flags == TVA_KHUGEPAGED) && vma_is_anonymous(vma))
		orders = THP_ORDERS_ALL_ANON;
	else
		orders = BIT(HPAGE_PMD_ORDER);

	return thp_vma_allowable_orders(vma, vm_flags, tva_flags, orders);
}

/* Return the highest naturally aligned order that fits at @offset within a PMD. */
static unsigned int max_order_from_offset(unsigned int offset)
{
	if (offset == 0)
		return HPAGE_PMD_ORDER;

	return min_t(unsigned int, __ffs(offset), HPAGE_PMD_ORDER);
}

/**
 * collapse_max_ptes_none - Calculate maximum allowed empty PTEs or PTEs mapping
 * the shared zeropage for the given collapse operation.
 * @cc: The collapse control struct
 * @vma: The vma to check for userfaultfd
 * @order: The folio order being collapsed to
 *
 * Return: Maximum number of empty/shared zeropage PTEs for the collapse operation
 */
unsigned int collapse_max_ptes_none(struct collapse_control *cc,
		struct vm_area_struct *vma, unsigned int order)
{
	const unsigned int max_ptes_none = cc->policy.max_ptes_none;

	if (vma && userfaultfd_armed(vma))
		return 0;
	/* The limit as given, at the PMD order and wherever it is not capped */
	if (is_pmd_order(order) || !cc->policy.strict_sub_pmd)
		return max_ptes_none;
	/*
	 * for mTHP collapse with the sysctl value set to COLLAPSE_MAX_PTES_LIMIT,
	 * scale the maximum number of PTEs to the order of the collapse.
	 */
	if (max_ptes_none == COLLAPSE_MAX_PTES_LIMIT)
		return (1 << order) - 1;
	/*
	 * For mTHP collapse of values other than 0 or COLLAPSE_MAX_PTES_LIMIT,
	 * emit a warning and return 0.
	 */
	if (max_ptes_none)
		pr_warn_once("mTHP collapse does not support max_ptes_none"
		     " values other than 0 or %u, defaulting to 0.\n",
		     COLLAPSE_MAX_PTES_LIMIT);
	return 0;
}

/**
 * collapse_max_ptes_shared - Calculate maximum allowed PTEs that map shared
 * anonymous pages for the given collapse operation.
 * @cc: The collapse control struct
 * @order: The folio order being collapsed to
 *
 * Return: Maximum number of PTEs that map shared anonymous pages for the
 * collapse operation
 */
static unsigned int collapse_max_ptes_shared(struct collapse_control *cc,
		unsigned int order)
{
	/*
	 * A sub-PMD window held to the strict rule takes no shared page at all:
	 * an mTHP is not worth the CoW-breaking.
	 */
	if (!is_pmd_order(order) && cc->policy.strict_sub_pmd)
		return 0;
	return cc->policy.max_ptes_shared;
}

/**
 * collapse_max_ptes_swap - Calculate the maximum allowed non-present PTEs or the
 * maximum allowed non-present pagecache entries for the given collapse operation.
 * @cc: The collapse control struct
 * @order: The folio order being collapsed to
 *
 * Return: Maximum number of non-present PTEs or the maximum allowed non-present
 * pagecache entries for the collapse operation.
 */
unsigned int collapse_max_ptes_swap(struct collapse_control *cc,
		unsigned int order)
{
	/*
	 * A sub-PMD window held to the strict rule takes nothing non-present:
	 * reading pages back to build an mTHP is not worth the latency.
	 */
	if (!is_pmd_order(order) && cc->policy.strict_sub_pmd)
		return 0;
	return cc->policy.max_ptes_swap;
}

bool collapse_scan_abort(int nid, struct collapse_control *cc)
{
	int i;

	/*
	 * If node_reclaim_mode is disabled, then no extra effort is made to
	 * allocate memory locally.
	 */
	if (!node_reclaim_enabled())
		return false;

	/* If there is a count for this node already, it must be acceptable */
	if (cc->node_load[nid])
		return false;

	for (i = 0; i < MAX_NUMNODES; i++) {
		if (!cc->node_load[i])
			continue;
		if (node_distance(nid, i) > node_reclaim_distance)
			return true;
	}
	return false;
}

#ifdef CONFIG_NUMA
int collapse_find_target_node(struct collapse_control *cc)
{
	int nid, target_node = 0, max_value = 0;

	/* find first node with max normal pages hit */
	for (nid = 0; nid < MAX_NUMNODES; nid++)
		if (cc->node_load[nid] > max_value) {
			max_value = cc->node_load[nid];
			target_node = nid;
		}

	for_each_online_node(nid) {
		if (max_value == cc->node_load[nid])
			node_set(nid, cc->alloc_nmask);
	}

	return target_node;
}
#else
int collapse_find_target_node(struct collapse_control *cc)
{
	return 0;
}
#endif
/*
 * The saved-PTE pool spans a whole table.  The byte cap bounds what a round
 * holds, but not what one candidate does: a sub-PMD order goes up to
 * HPAGE_PMD_NR/2 pages -- 256M at order 12 with 64K pages -- and displaces all
 * of its PTEs in one shot regardless.  So the pool has to fit the largest span
 * of displaced PTEs a table can hold, which is the table itself.
 */
#define COLLAPSE_SAVED_PTES	HPAGE_PMD_NR

/*
 * Capacity of the retry store: the most regions a table can hold at once.  Live
 * entries cover disjoint regions -- a region is one candidate's extent, and an
 * extent is consumed from the cursor or from one entry, never from two -- and
 * the smallest a producer pushes is one window at the smallest order.  Not
 * bounded by what a round pushes: the stack is drained from the top, so an entry
 * below a live one outlives the round that pushed it.
 */
#define COLLAPSE_RETRY_STORE_SIZE	COLLAPSE_TABLE_WINDOWS

/* How far a candidate got, and so what a failure has to undo for it */
enum collapse_candidate_state {
	CAND_SELECTED,		/* collected; nothing held on its behalf yet */
	CAND_SKIPPED,		/* refused; nothing of it left to undo */
	CAND_FROZEN,		/* sources displaced and frozen */
	CAND_INSTALLED,		/* the destination is mapped */
};

/*
 * A region queued to re-enter selection, walked like the table itself: @offset is
 * the next window to probe, @end one past the region, and @order the largest to
 * try -- below the order that just failed, so the same window cannot be emitted
 * twice.  Walking the region rather than shrinking one window keeps its tail,
 * which is often where the collapsible window is.
 */
struct collapse_retry {
	unsigned int offset;
	unsigned int end;
	unsigned int order;
	/* The light allocation missed here: the next attempt may reclaim */
	bool reclaim;
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
	pte_t *saved_ptes;		/* its slice of collapse_control::saved_ptes */
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

static unsigned int candidate_nr_pages(const struct collapse_candidate *cand)
{
	return 1U << cand->order;
}

static void collapse_trace_candidate(struct mm_struct *mm,
				     const struct collapse_candidate *cand,
				     enum collapse_pass pass)
{
	trace_mm_collapse_candidate(mm, cand->addr, cand->order, pass,
				    cand->result);
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
	kfree(cc->saved_ptes);
	kfree(cc->retries);
	cc->candidates = NULL;
	cc->saved_ptes = NULL;
	cc->retries = NULL;
}

int collapse_control_init(struct collapse_control *cc)
{
	cc->nr_candidates = 0;
	cc->nr_retries = 0;
	cc->candidates = kmalloc_objs(*cc->candidates, COLLAPSE_MAX_CANDIDATES);
	cc->saved_ptes = kmalloc_objs(*cc->saved_ptes, COLLAPSE_SAVED_PTES);
	cc->retries = kmalloc_objs(*cc->retries, COLLAPSE_RETRY_STORE_SIZE);
	if (!cc->candidates || !cc->saved_ptes || !cc->retries) {
		collapse_control_release(cc);
		return -ENOMEM;
	}
	return 0;
}

/*
 * The scan and the allocation both dropped mmap_lock, so nothing seen before it
 * can be trusted: check the VMA the round just looked up and the PTE table
 * again, and that they still allow every provisioned candidate.
 *
 * The VMA was found by address, so it need not be the one the scan saw, nor
 * still cover everything the round collected -- thp_vma_suitable_order() asks
 * that of each candidate, since a window is aligned to its own order.  A VMA
 * that shrank under a candidate therefore refuses that candidate and no more,
 * like every other pass.
 *
 * What survives is what the round goes on to freeze, so this is also where the
 * batch's span is settled, for the invalidate the round issues over it.
 */
static enum scan_result collapse_revalidate(struct vm_area_struct *vma,
					    unsigned long pmd_addr,
					    struct collapse_control *cc,
					    pmd_t **pmdp)
{
	struct mm_struct *mm = vma->vm_mm;
	enum scan_result result;
	unsigned int i, nr_live = 0;

	if (unlikely(collapse_test_exit_or_disable(mm)))
		return SCAN_ANY_PROCESS;

	if (!vma->anon_vma || !vma_is_anonymous(vma))
		return SCAN_PAGE_ANON;

	result = find_pmd_or_thp_or_none(mm, pmd_addr, pmdp);
	if (result != SCAN_SUCCEED)
		return result;

	cc->batch_start = ULONG_MAX;
	cc->batch_end = 0;

	for (i = 0; i < cc->nr_candidates; i++) {
		struct collapse_candidate *cand = &cc->candidates[i];

		if (cand->state != CAND_SELECTED)
			continue;

		/*
		 * The window has to still fit the VMA, which may have shrunk or
		 * been replaced, and its order to still be one the VMA allows.
		 */
		if (!thp_vma_suitable_order(vma, cand->addr, cand->order) ||
		    !thp_vma_allowable_orders(vma, vma->vm_flags,
					      cc->policy.tva_type,
					      BIT(cand->order))) {
			cand->state = CAND_SKIPPED;
			cand->result = SCAN_VMA_CHECK;
			collapse_trace_candidate(mm, cand,
						 COLLAPSE_PASS_REVALIDATE);
			continue;
		}

		cc->batch_start = min(cc->batch_start, candidate_start(cand));
		cc->batch_end = max(cc->batch_end, candidate_end(cand));
		nr_live++;
	}

	/* Nothing the VMA still allows: no span to invalidate, nothing to run */
	if (!nr_live)
		return SCAN_VMA_CHECK;

	return SCAN_SUCCEED;
}

/*
 * Faults one address may take before the freeze is left to judge it.  More than
 * one because the unshare can race a co-mapper re-sharing the page, and a swap
 * read can be interrupted; each try re-reads the PTE to see where it stands.
 */
#define COLLAPSE_FAULTIN_TRIES	3

/*
 * Bring one address to a state the freeze will accept: present, and exclusive if
 * it is anonymous.  Returns with mmap_lock dropped on every failure, because the
 * fault path may drop it and the caller cannot tell which case it is in.
 *
 * SCAN_EXCEED_SWAP_PTE is the exception: it is a verdict on this candidate
 * rather than on the round, nothing was faulted to reach it, and it keeps the
 * lock so the caller can refuse this candidate and carry on with the rest.
 */
static enum scan_result collapse_faultin_addr(struct vm_area_struct *vma,
					      struct collapse_candidate *cand,
					      pmd_t *pmd, unsigned long addr)
{
	struct mm_struct *mm = vma->vm_mm;
	const unsigned int flags = FAULT_FLAG_ALLOW_RETRY | FAULT_FLAG_UNSHARE |
		(mm != current->mm ? FAULT_FLAG_REMOTE : 0);
	unsigned int tries;

	for (tries = 0; tries <= COLLAPSE_FAULTIN_TRIES; tries++) {
		struct page *page;
		pte_t ptent, *pte;
		vm_fault_t ret;

		pte = pte_offset_map(pmd, addr);
		if (!pte) {
			mmap_read_unlock(mm);
			return SCAN_NO_PTE_TABLE;
		}
		ptent = ptep_get_lockless(pte);
		pte_unmap(pte);

		/* A hole or the zeropage is population's business */
		if (pte_none_or_zero(ptent))
			break;

		if (pte_present(ptent)) {
			page = vm_normal_page(vma, addr, ptent);

			/*
			 * PageAnonExclusive is the invariant the freeze relies
			 * on, and the only exact test for it.  Testing sharing
			 * with folio_maybe_mapped_shared() is not the same: a
			 * page whose fork co-mapper has gone away is
			 * single-mapped, yet stays non-exclusive until a write
			 * reuses it, so sharing would skip the unshare on
			 * exactly the pages that need it.  Unsharing one of
			 * those is cheap -- it reuses the page in place and
			 * just sets the bit.
			 */
			if (!page || !folio_test_anon(page_folio(page)) ||
			    PageAnonExclusive(page))
				break;		/* already exclusive */
		} else if (!is_pmd_order(cand->order)) {
			/* Sub-PMD collapse does not fault swap in */
			count_mthp_stat(cand->order,
					MTHP_STAT_COLLAPSE_EXCEED_SWAP);
			return SCAN_EXCEED_SWAP_PTE;
		}

		if (tries == COLLAPSE_FAULTIN_TRIES)
			break;		/* the freeze refuses it if still unfit */

		/* Only swap or shared PTEs reach here; the rest broke out */
		ret = handle_mm_fault(vma, addr, flags, NULL);
		/*
		 * Not a verdict on this window: the fault dropped the lock to
		 * wait, which is what a swap-in normally does.  Distinct from
		 * SCAN_PAGE_LOCK, a folio someone else holds locked.
		 */
		if (ret & VM_FAULT_RETRY)
			return SCAN_LOCK_DROPPED;
		if (ret & VM_FAULT_ERROR) {
			mmap_read_unlock(mm);
			return SCAN_FAIL;
		}
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
	enum scan_result result = SCAN_SUCCEED;
	unsigned int i;

	for (i = 0; i < cc->nr_candidates; i++) {
		struct collapse_candidate *cand = &cc->candidates[i];
		unsigned long addr;
		unsigned int j;

		if (cand->state != CAND_SELECTED)
			continue;

		for (j = 0, addr = cand->addr;
		     j < candidate_nr_pages(cand);
		     j++, addr += PAGE_SIZE) {
			enum scan_result r;

			r = collapse_faultin_addr(vma, cand, pmd, addr);
			/*
			 * The one failure that judges this candidate rather
			 * than the round, and so the one that leaves the lock
			 * in our hands: refuse it and go on to the next.
			 * Failing the round here would lower the order of every
			 * candidate it carries, a verdict nobody reached.
			 */
			if (r == SCAN_EXCEED_SWAP_PTE) {
				cand->state = CAND_SKIPPED;
				cand->result = r;
				collapse_trace_candidate(vma->vm_mm, cand,
							 COLLAPSE_PASS_FAULTIN);
				break;
			}
			if (r != SCAN_SUCCEED) {
				result = r;
				goto out;
			}
		}
	}
out:
	return result;
}

/*
 * How many slots a source span starting at @first may cover: the pages left in
 * its folio, capped at @max.  Every freeze-side walker bounds spans with this,
 * so per-span batching of clears, locks and freezes cannot reach a slot the span
 * does not cover.
 */
static unsigned int collapse_span_max(pte_t first, unsigned int max)
{
	struct page *page = pte_page(first);
	struct folio *folio = page_folio(page);
	unsigned int left = folio_nr_pages(folio) - folio_page_idx(folio, page);

	return min(max, left);
}

/*
 * Length of the source span at slot @i, read from the saved PTEs rather than the
 * table: once frozen the slots hold migration entries, so a rollback re-derives
 * the freeze's spans from what it displaced.
 */
static unsigned int collapse_saved_span_len(struct collapse_candidate *cand,
					    unsigned int i, unsigned int bound)
{
	pte_t first = cand->saved_ptes[i];
	unsigned int nr, nr_max;

	nr_max = collapse_span_max(first, bound - i);
	for (nr = 1; nr < nr_max; nr++) {
		pte_t saved = cand->saved_ptes[i + nr];

		if (pte_none_or_zero(saved) ||
		    pte_pfn(saved) != pte_pfn(first) + nr)
			break;
	}
	return nr;
}

/*
 * Undo a freeze that could not complete: restore the displaced PTE values over
 * the candidate's migration entries, then unfreeze, unlock and release the
 * source folios.
 *
 * How far the freeze got:
 *
 *  - @nr_saved slots were displaced, in PTEs;
 *  - @nr_frozen of those belong to folios that were also frozen.
 *
 * Each slot restores by class: a hole was never modified, a cleared zeropage is
 * stored back plainly, and a source's saved value goes back as it was.  All are
 * plain stores -- writing over a non-present entry has no hardware A/D race.
 *
 * Slot by slot, not one set_ptes() over the span: the PTEs of one folio need
 * not agree on more than the PFN, so the first one's permissions are not the
 * span's.
 *
 * Deliberately no TLB flush: the restored translation is identical to anything
 * a stale TLB entry may hold, so every stale entry is benign.  This reads like
 * a missing flush; it is not.
 *
 * Caller holds the table's ptl -- the same uninterrupted hold the freeze ran
 * under.
 */
static void collapse_unfreeze_candidate(struct mm_struct *mm,
					struct collapse_candidate *cand,
					pte_t *pte, unsigned int nr_saved,
					unsigned int nr_frozen)
{
	unsigned long addr = cand->addr;
	unsigned int i = 0;

	while (i < nr_saved) {
		pte_t saved = cand->saved_ptes[i];
		struct folio *folio;
		unsigned int nr, k;

		if (pte_none(saved)) {
			/* Hole: nothing was touched */
			i++;
			addr += PAGE_SIZE;
			continue;
		}
		if (is_zero_pfn(pte_pfn(saved))) {
			/* Cleared zeropage: plain non-present -> present store */
			set_pte_at(mm, addr, pte + i, saved);
			i++;
			addr += PAGE_SIZE;
			continue;
		}

		folio = pte_folio(saved);
		nr = collapse_saved_span_len(cand, i, nr_saved);

		for (k = 0; k < nr; k++) {
			set_pte_at(mm, addr + k * PAGE_SIZE, pte + i + k,
				   cand->saved_ptes[i + k]);
		}
		if (i < nr_frozen) {
			folio_ref_unfreeze(folio,
					   folio_expected_ref_count(folio) + 1);
		}
		folio_unlock(folio);
		folio_put(folio);

		i += nr;
		addr += nr * PAGE_SIZE;
	}
}

/*
 * Can this candidate's sources be frozen?  Every slot is checked and nothing is
 * touched, so a refusal costs the round nothing but the walk.
 *
 * The walk is in source spans: a span is consecutive PTEs mapping consecutive
 * pages of one folio, and it ends wherever the next PTE stops being the folio's
 * next page.  No layout is refused for its shape -- the next slot simply starts
 * its own span -- so partially mapped and compound sources collapse too.
 *
 * Caller holds mmap_read and the table's ptl.
 */
static enum scan_result collapse_check_candidate(struct vm_area_struct *vma,
						 struct collapse_control *cc,
						 struct collapse_candidate *cand,
						 pte_t *pte)
{
	const unsigned int nr_pages = candidate_nr_pages(cand);
	unsigned long addr;
	unsigned int i;

	for (i = 0, addr = cand->addr; i < nr_pages;) {
		pte_t ptent = ptep_get(pte + i);
		unsigned int nr, nr_max, k;
		struct folio *folio;
		struct page *page;

		if (!pte_present(ptent)) {
			/* Holes are population; swap and markers are not */
			if (pte_none(ptent)) {
				i++;
				addr += PAGE_SIZE;
				continue;
			}
			return SCAN_PTE_NON_PRESENT;
		}
		if (pte_uffd(ptent))
			return SCAN_PTE_UFFD;

		/* The zeropage zero-fills like a hole, and has no normal page */
		if (is_zero_pfn(pte_pfn(ptent))) {
			i++;
			addr += PAGE_SIZE;
			continue;
		}
		page = vm_normal_page(vma, addr, ptent);
		if (!page || unlikely(is_zone_device_page(page)))
			return SCAN_PAGE_NULL;

		folio = page_folio(page);
		if (!folio_test_anon(folio))
			return SCAN_PAGE_ANON;

		/*
		 * Collapsing a MADV_FREE'd page would copy it into a folio that
		 * is not lazyfree, quietly making memory the user offered up
		 * undroppable again.
		 */
		if (cc->policy.skip_lazyfree &&
		    !(vma->vm_flags & VM_DROPPABLE) &&
		    folio_test_lazyfree(folio) && !pte_dirty(ptent))
			return SCAN_PAGE_LAZYFREE;

		/*
		 * A sub-PMD candidate refuses folios of its own order and above:
		 * collapsing those would gain nothing.  A PMD candidate accepts
		 * every order up to its own -- the PTE-mapped-THP re-collapse
		 * class.
		 */
		if (folio_order(folio) >= cand->order &&
		    !is_pmd_order(cand->order))
			return SCAN_PTE_MAPPED_HUGEPAGE;

		/*
		 * Exclusive anon only: the expected refcount of a shared folio
		 * cannot be pinned down without its other mappers' ptls.
		 * Swapcache membership is fine -- folio_expected_ref_count()
		 * accounts those references.
		 */
		if (folio_maybe_mapped_shared(folio))
			return SCAN_PAGE_NOT_EXCLUSIVE;

		nr_max = collapse_span_max(ptent, nr_pages - i);
		for (nr = 1; nr < nr_max; nr++) {
			pte_t tail = ptep_get(pte + i + nr);

			if (!pte_present(tail) ||
			    pte_pfn(tail) != pte_pfn(ptent) + nr)
				break;
			if (pte_uffd(tail))
				return SCAN_PTE_UFFD;
		}

		/*
		 * Every live mapping of the folio must be this span: the freeze
		 * is whole-folio, and a live PTE left anywhere else loses to a
		 * racing zap -- its rmap drop is paired with a folio_put() that
		 * would underflow the frozen count.  The check is race-free
		 * under our ptl: in-window PTEs are ours, fork (the only way
		 * exclusive anon gains mappings) takes mmap_write, and a folio
		 * whose mappings all sit under this ptl cannot lose one either.
		 * This also refuses a folio scattered across several spans of
		 * the window, whose mapcount exceeds any single span.
		 */
		if (folio_mapcount(folio) != nr)
			return SCAN_PAGE_COUNT;

		/*
		 * Every page of the span must be exclusive: the freeze accounts
		 * only references it can see, and a non-exclusive page may be
		 * unshared under us.  collapse_faultin() should have arranged
		 * this; enforce it here, where it is depended on.
		 */
		for (k = 0; k < nr; k++) {
			if (!PageAnonExclusive(pte_page(ptep_get(pte + i + k))))
				return SCAN_PAGE_NOT_EXCLUSIVE;
		}

		i += nr;
		addr += nr * PAGE_SIZE;
	}

	return SCAN_SUCCEED;
}

/*
 * Freeze one candidate's sources, span by span, raising both quiescence
 * barriers in reachability order:
 *
 *  1. the span's PTEs become migration entries.  Faults and GUP-slow now wait
 *     on the source folio's lock, taken before the first entry is visible.
 *  2. the folio is frozen to its expected reference count, so folio_try_get()
 *     fails for anyone taking a speculative reference.
 *
 * All or nothing: a failure part way through unwinds what it displaced and
 * leaves the table as it was found.
 *
 * Neither barrier deflects a path that takes its reference outright rather
 * than speculatively.  Such a source has to be refused before the freeze, not
 * survive it -- see the writeback test below.
 *
 * A round holds every source folio's lock at once, from freeze to putback, and
 * folio locks have no global order.  That cannot deadlock: folio_trylock() is
 * the engine's only acquisition and a refusal unfreezes instead of blocking, so
 * the engine is never the waiting edge of a cycle.  Nothing between freeze and
 * putback waits on anything that could wait on us -- allocation and charging
 * happen earlier, and the copy only copies.  The install does take the ptl
 * while holding these folio locks, which is the safe order: a faulter on one of
 * our migration entries cannot sleep on the folio lock under a spinlock, so it
 * drops the ptl first.  Do not add a blocking lock or a sleeping allocation
 * between freeze and putback.
 *
 * On entry:
 *
 *  - mmap_read is held, and the table's ptl for the whole freeze;
 *  - collapse_check_candidate() has accepted the candidate under that same ptl
 *    hold;
 *  - the round is covered by an mmu_notifier_invalidate_range_start() issued
 *    outside the ptl.
 *
 * collapse_freeze() issues the ranged TLB flush over everything that froze
 * before dropping the ptl.  No copy may run before it completes.
 */
static enum scan_result collapse_freeze_candidate(struct mm_struct *mm,
		struct collapse_candidate *cand, pte_t *pte)
{
	const unsigned int nr_pages = candidate_nr_pages(cand);
	unsigned int nr_saved = 0, nr_frozen = 0;
	enum scan_result result;
	struct folio *folio;
	unsigned long addr;
	unsigned int i;

	for (i = 0, addr = cand->addr; i < nr_pages;) {
		pte_t ptent = ptep_get(pte + i);
		unsigned int nr, nr_max, k;
		pte_t rep;

		if (pte_none(ptent)) {
			/* Hole: nothing to freeze; install verifies it stayed one */
			cand->saved_ptes[i] = ptent;
			nr_saved = ++i;
			addr += PAGE_SIZE;
			continue;
		}
		if (is_zero_pfn(pte_pfn(ptent))) {
			/*
			 * Clear the zeropage mapping now, covered by the round's
			 * ranged flush: overwriting a live PTE at install would
			 * be a valid->valid transition, breaking arm64's
			 * break-before-make.  The zeropage has neither rmap nor
			 * per-map references -- the saved value alone undoes it.
			 */
			cand->saved_ptes[i] =
				ptep_get_and_clear(mm, addr, pte + i);
			nr_saved = ++i;
			addr += PAGE_SIZE;
			continue;
		}

		folio = pte_folio(ptent);

		/*
		 * A folio revisited by a second span of this round is already
		 * ours and frozen at its first span: folio_get() on a zero count
		 * is a bug, and try-get fails cleanly.  Scrambled layouts
		 * (mremap) construct this; nothing else can hold a folio frozen
		 * while its PTE is live under our ptl, so it is not transient.
		 */
		if (!folio_try_get(folio)) {
			result = SCAN_PAGE_COUNT;
			goto unfreeze;
		}
		if (!folio_trylock(folio)) {
			folio_put(folio);
			result = SCAN_PAGE_LOCK;
			goto unfreeze;
		}

		/*
		 * Never freeze a folio under writeback.  PG_writeback holds no
		 * reference of its own -- the swapcache reference keeps the folio
		 * alive, and everything that would drop it waits for the flag --
		 * so folio_end_writeback() plain folio_get()s a folio it may
		 * assume is alive: a BUG on a frozen one, or with
		 * CONFIG_DEBUG_VM off, a free under our copy.
		 *
		 * Unlike every other hazard here, the freeze does not catch it.
		 * folio_expected_ref_count() counts the swapcache reference, so
		 * the count is exactly right and the freeze succeeds.  Nor can
		 * "is it in the swapcache" stand in for this test: that would
		 * refuse the pages the fault-in pass just swapped in.
		 *
		 * Reachable even though writeback starts on an unmapped folio: a
		 * re-fault from the swapcache maps it back before the bio
		 * completes, and folio_free_swap() will not drop the cache entry
		 * under writeback.  Testing once is enough -- writeback starts
		 * only under the folio lock, which we hold from here through
		 * putback.
		 */
		if (folio_test_writeback(folio)) {
			folio_unlock(folio);
			folio_put(folio);
			result = SCAN_PAGE_DIRTY_OR_WRITEBACK;
			goto unfreeze;
		}

		/* Each slot's own value: a span agrees on the PFN, not the rest */
		cand->saved_ptes[i] = ptent;
		nr_max = collapse_span_max(ptent, nr_pages - i);
		for (nr = 1; nr < nr_max; nr++) {
			pte_t tail = ptep_get(pte + i + nr);

			if (!pte_present(tail) ||
			    pte_pfn(tail) != pte_pfn(ptent) + nr)
				break;
			cand->saved_ptes[i + nr] = tail;
		}

		/*
		 * The clear is the GUP-fast linearization point: a grab landing
		 * before it elevates the refcount and the freeze below fails
		 * (the candidate unfreezes); one landing after fails its PTE
		 * re-read and retries.  Clear and store sit adjacent under one
		 * uninterrupted ptl hold, batched per span
		 * (get_and_clear_full_ptes() unfolds contpte), so the transient
		 * none window is invisible to installers, which all take the ptl.
		 */
		rep = get_and_clear_full_ptes(mm, addr, pte + i, nr, 0);

		/*
		 * Dirty from the clear -- including any the hardware set since
		 * the reads above -- goes to the folio, the way unmap does,
		 * rather than onto PTEs that never had it.  Young needs no such
		 * care: a migration entry drops it either way.
		 */
		if (pte_dirty(rep))
			folio_mark_dirty(folio);

		for (k = 0; k < nr; k++) {
			pte_t saved = cand->saved_ptes[i + k];
			swp_entry_t entry;
			pte_t swp_pte;

			entry = make_readable_migration_entry(pte_pfn(saved));
			swp_pte = swp_entry_to_pte(entry);
			if (pte_soft_dirty(saved))
				swp_pte = pte_swp_mksoft_dirty(swp_pte);
			set_pte_at(mm, addr + k * PAGE_SIZE, pte + i + k,
				   swp_pte);
		}
		nr_saved = i + nr;

		if (!folio_ref_freeze(folio,
				      folio_expected_ref_count(folio) + 1)) {
			result = SCAN_PAGE_COUNT;
			goto unfreeze;
		}
		nr_frozen = nr_saved;

		i += nr;
		addr += nr * PAGE_SIZE;
	}

	cand->state = CAND_FROZEN;
	return SCAN_SUCCEED;

unfreeze:
	collapse_unfreeze_candidate(mm, cand, pte, nr_saved, nr_frozen);
	return result;
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
	unsigned long flush_start = ULONG_MAX, flush_end = 0;
	struct mm_struct *mm = vma->vm_mm;
	pte_t *pte, *table;
	spinlock_t *ptl;
	unsigned int i;

	/* Pending per-CPU folio batches hold references that fail the freeze */
	lru_add_drain();

	pte = pte_offset_map_lock(mm, pmd, cc->candidates[0].addr, &ptl);
	if (!pte) {
		for (i = 0; i < cc->nr_candidates; i++) {
			struct collapse_candidate *cand = &cc->candidates[i];

			if (cand->state != CAND_SELECTED)
				continue;
			cand->state = CAND_SKIPPED;
			cand->result = SCAN_NO_PTE_TABLE;
			collapse_trace_candidate(mm, cand, COLLAPSE_PASS_FREEZE);
		}
		return;
	}

	/*
	 * Index each candidate from the table base, not relative to
	 * candidates[0]: a round is not necessarily address-ordered, so
	 * candidates[0] need not be the lowest.  They all share one table.
	 */
	table = pte - pte_index(cc->candidates[0].addr);

	for (i = 0; i < cc->nr_candidates; i++) {
		struct collapse_candidate *cand = &cc->candidates[i];
		pte_t *cand_pte = table + pte_index(cand->addr);
		enum scan_result result;

		if (cand->state != CAND_SELECTED)
			continue;

		result = collapse_check_candidate(vma, cc, cand, cand_pte);
		if (result == SCAN_SUCCEED)
			result = collapse_freeze_candidate(mm, cand, cand_pte);

		cand->result = result;
		if (result != SCAN_SUCCEED) {
			cand->state = CAND_SKIPPED;
			collapse_trace_candidate(mm, cand, COLLAPSE_PASS_FREEZE);
			continue;
		}

		flush_start = min(flush_start, candidate_start(cand));
		flush_end = max(flush_end, candidate_end(cand));
	}

	if (flush_end)
		flush_tlb_range(vma, flush_start, flush_end);
	pte_unmap_unlock(pte, ptl);
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
		collapse_trace_candidate(mm, cand, COLLAPSE_PASS_ALLOC);
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
		collapse_trace_candidate(mm, cand, COLLAPSE_PASS_ALLOC);
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

		if (cand->state != CAND_FROZEN || cand->new_folio)
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

		collapse_trace_candidate(mm, cand, COLLAPSE_PASS_ALLOC);
	}
}

/*
 * Copy the frozen sources into their destinations.  Nothing can reach either
 * side, so this needs no page-table lock, and it sleeps.
 */
static void collapse_copy(struct vm_area_struct *vma,
			  struct collapse_control *cc)
{
	unsigned int i;

	for (i = 0; i < cc->nr_candidates; i++) {
		struct collapse_candidate *cand = &cc->candidates[i];
		const unsigned int nr_pages = candidate_nr_pages(cand);
		unsigned long addr = cand->addr;
		unsigned int k;

		/* A folio does not imply a freeze: reserve runs before the lock */
		if (cand->state != CAND_FROZEN)
			continue;

		/* A freeze does not imply a folio: provision may have declined */
		if (!cand->new_folio)
			continue;

		/* Each source lands where its address puts it: slot k, page k */
		for (k = 0; k < nr_pages; k++, addr += PAGE_SIZE) {
			struct page *dst = folio_page(cand->new_folio, k);
			struct page *src;

			/* No source: a hole, or a zeropage the freeze cleared */
			if (pte_none_or_zero(cand->saved_ptes[k])) {
				clear_user_highpage(dst, addr);
				continue;
			}

			src = pte_page(cand->saved_ptes[k]);

			/*
			 * A machine check on a source is the only way this
			 * fails, and the install is what undoes the candidate:
			 * that is where the ptl the undoing needs is held.
			 */
			if (copy_mc_user_highpage(dst, src, addr, vma)) {
				cand->result = SCAN_COPY_MC;
				collapse_trace_candidate(vma->vm_mm, cand,
							 COLLAPSE_PASS_COPY);
				break;
			}
		}
	}
}

/*
 * Undo one frozen slot: restore the saved PTE if our migration entry is still
 * there, or drop the rmap the freeze took if a racing zap already replaced it.
 * Returns true when the slot was zapped -- its mapping reference is then ours to
 * release.
 */
static bool collapse_abort_slot(struct vm_area_struct *vma, struct folio *folio,
				pte_t *slot, unsigned long addr, pte_t saved)
{
	/* No table left: the slot cannot still be holding our entry */
	if (slot) {
		softleaf_t entry = softleaf_from_pte(ptep_get(slot));

		if (softleaf_is_migration(entry) &&
		    softleaf_to_pfn(entry) == pte_pfn(saved)) {
			set_pte_at(vma->vm_mm, addr, slot, saved);
			return false;
		}
	}
	folio_remove_rmap_pte(folio, pte_page(saved), vma);
	return true;
}

/*
 * Abort one frozen candidate at install time: it took a machine check during the
 * copy, or some of its slots no longer hold our migration entries.  mmap_read
 * (held freeze..putback) blocks fork, mremap and munmap, and faults wait on the
 * migration entries -- but madvise-class operations run under mmap_read too, so a
 * concurrent MADV_DONTNEED may have zapped frozen slots, and a fault may have
 * refilled a zapped one.
 *
 * Slots still holding our entries are restored from the saved values (no TLB
 * flush: identical translation).  Foreign slots are left exactly as found --
 * restoring them would resurrect memory the user zapped -- but their rmap is
 * dropped here: the zapper fixed up rss for the slots it cleared, yet could not
 * drop the rmap a frozen source keeps, unlike a migrating one, which unmaps at
 * freeze time.  Slots with no source follow the same rule with no rmap to drop: a
 * cleared zeropage is restored only while its slot is still none, and a hole was
 * never touched at all.
 *
 * @pte is NULL when the table itself is gone: a racing whole-table MADV_DONTNEED
 * zapped every entry, frozen slots included, and the empty-table reclaim
 * (CONFIG_PT_RECLAIM) freed it, clearing the pmd under the pmd lock and the pte
 * ptl, neither of which excludes it between our freeze and install.  Every slot
 * then reads as foreign, which is exactly right: nothing of ours survives to
 * restore or verify, and what is left is the half of the teardown the zapper
 * cannot perform for a frozen source -- the kept rmap, the freeze, the folio
 * locks and the references.  The caller holds no page-table lock in that case,
 * there being no table to lock.
 */
static void collapse_abort_candidate(struct vm_area_struct *vma,
				     struct collapse_candidate *cand,
				     pte_t *pte)
{
	const unsigned int nr_pages = candidate_nr_pages(cand);
	struct mm_struct *mm = vma->vm_mm;
	unsigned long addr = cand->addr;
	unsigned int i, nr;

	for (i = 0; i < nr_pages; i += nr, addr += nr * PAGE_SIZE) {
		pte_t saved = cand->saved_ptes[i];
		unsigned int k, nr_dropped;
		struct folio *folio;

		nr = 1;		/* skip stride; a span overrides it */
		if (pte_none(saved))
			continue;
		if (is_zero_pfn(pte_pfn(saved))) {
			if (pte && pte_none(ptep_get(pte + i)))
				set_pte_at(mm, addr, pte + i, saved);
			continue;
		}

		folio = pte_folio(saved);
		nr = collapse_saved_span_len(cand, i, nr_pages);

		/*
		 * Unfreeze before any rmap drop: rmap removal munlocks under
		 * VM_LOCKED, and munlock_folio() takes a reference a frozen folio
		 * forbids.  The expected count still holds every slot's mapping
		 * reference; restored slots keep theirs, and the zapped slots'
		 * references become ours to drop with the rmap.
		 */
		folio_ref_unfreeze(folio, folio_expected_ref_count(folio) + 1);

		nr_dropped = 0;
		for (k = 0; k < nr; k++) {
			pte_t *slot = pte ? pte + i + k : NULL;

			nr_dropped += collapse_abort_slot(vma, folio, slot,
							  addr + k * PAGE_SIZE,
							  cand->saved_ptes[i + k]);
		}

		folio_unlock(folio);
		folio_put_refs(folio, nr_dropped + 1);
	}

	/*
	 * Not installed; collapse_finish() releases the destination, which has to
	 * wait for the ptl to be dropped.
	 */
	cand->state = CAND_SKIPPED;
}

/*
 * Nothing may have shifted under the round: every source slot must still hold our
 * migration entry, and every slot with no source must still be none -- the freeze
 * cleared the zeropage ones, so both read as none by then, and a slot some fault
 * has refilled, with a page or with a zeropage, is not ours to overwrite.
 * @nr_populated returns how many source-less slots the install is about to make
 * present, which is rss no zap ever accounted for.
 */
static bool collapse_verify_candidate(struct collapse_candidate *cand,
				      pte_t *pte, unsigned int *nr_populated)
{
	const unsigned int nr_pages = candidate_nr_pages(cand);
	unsigned int k, populated = 0;

	for (k = 0; k < nr_pages; k++) {
		pte_t live = ptep_get(pte + k);
		softleaf_t entry;

		if (pte_none_or_zero(cand->saved_ptes[k])) {
			if (!pte_none(live))
				return false;
			populated++;
			continue;
		}

		entry = softleaf_from_pte(live);
		if (!softleaf_is_migration(entry) ||
		    softleaf_to_pfn(entry) != pte_pfn(cand->saved_ptes[k]))
			return false;
	}
	*nr_populated = populated;
	return true;
}

/*
 * The PMD terminal layer: verify, detach the table, deposit a fresh one and
 * install the leaf, as one atomic section under the pmd lock.  A pmd_none window
 * never exists -- faults stay held at pte level by the migration entries
 * throughout -- which is what lets PMD collapse run under mmap_read like the rest
 * of the engine.
 */
static void collapse_install_pmd(struct vm_area_struct *vma,
				 struct collapse_control *cc, pmd_t *pmd)
{
	struct collapse_candidate *cand = &cc->candidates[0];
	struct mm_struct *mm = vma->vm_mm;
	spinlock_t *pmd_ptl, *pte_ptl;
	pgtable_t old_table = NULL;
	unsigned int nr_populated;
	pmd_t old_pmd, pmdval;
	pte_t *pte;

	if (cand->state != CAND_FROZEN)
		return;

	/* No destination: the provision pass could not spare one */
	if (!cand->new_folio) {
		pte = pte_offset_map_lock(mm, pmd, cand->addr, &pte_ptl);
		collapse_abort_candidate(vma, cand, pte);
		if (pte)
			pte_unmap_unlock(pte, pte_ptl);
		return;
	}

	/*
	 * The pte ptl nests inside the pmd lock, the nesting the tree already
	 * uses for reinstalling a table: a racing zap of a frozen entry takes
	 * the pte ptl, so the verify must hold it, and the table must not come
	 * apart between verify and detach.  pmd_same() rechecks are unnecessary,
	 * the pmd lock being held across the whole section.
	 */
	pmd_ptl = pmd_lock(mm, pmd);
	pte = pte_offset_map_rw_nolock(mm, pmd, cand->addr, &pmdval, &pte_ptl);
	if (!pte) {
		/* Table gone under us; see collapse_abort_candidate() on @pte */
		spin_unlock(pmd_ptl);
		cand->result = SCAN_NO_PTE_TABLE;
		collapse_trace_candidate(mm, cand, COLLAPSE_PASS_INSTALL);
		collapse_abort_candidate(vma, cand, NULL);
		return;
	}
	if (pte_ptl != pmd_ptl)
		spin_lock_nested(pte_ptl, SINGLE_DEPTH_NESTING);

	/*
	 * Every exit is inside that section, the aborts as much as the install.
	 * An abort needs no pmd-level exclusion of its own; it only restores
	 * PTEs.  But the table it works on came from pte_offset_map_rw_nolock(),
	 * which leaves its caller to establish that the pmd is stable, and the
	 * held pmd lock is what does that here.
	 */
	if (cand->result != SCAN_SUCCEED) {
		/* Machine check during the copy */
		collapse_abort_candidate(vma, cand, pte);
		goto out_unlock;
	}

	if (!collapse_verify_candidate(cand, pte, &nr_populated)) {
		cand->result = SCAN_PTE_NON_PRESENT;
		collapse_trace_candidate(mm, cand, COLLAPSE_PASS_INSTALL);
		collapse_abort_candidate(vma, cand, pte);
		goto out_unlock;
	}

	/*
	 * Nothing fallible sits past here.  No anon_vma_lock_write either: rmap
	 * walks on the sources are unreachable -- refcounts frozen, folio locks
	 * held from freeze to putback -- non-rmap pte walkers see migration
	 * entries, pmd-level observers see the old table or the leaf and never an
	 * intermediate, and fork, mremap and munmap take mmap_write, which our
	 * mmap_read excludes.
	 *
	 * The flush inside pmdp_collapse_flush() is the round's second over this
	 * range: the freeze displaced every leaf here and flushed before dropping
	 * the ptl, and the verify above proved nothing has been mapped since.
	 * What it covers is the paging-structure caches -- a CPU may still hold
	 * the pmd-to-table link, for a table that is about to be freed -- which
	 * is why the helper shoots down a pte range rather than a pmd.
	 */
	old_pmd = pmdp_collapse_flush(vma, cand->addr, pmd);
	old_table = pmd_pgtable(old_pmd);

	/*
	 * The smp_wmb() in __folio_mark_uptodate() orders the copied data before
	 * the install below publishes it.
	 */
	__folio_mark_uptodate(cand->new_folio);

	/*
	 * Deposit a freshly allocated table, not the one just detached: a
	 * deposited table has to be quiescent, because whoever withdraws it frees
	 * it immediately (zap_huge_pmd()) with nothing to hold a lockless walker
	 * off first.  A table that has never been reachable is quiescent by
	 * construction, which is why collapse_alloc() secured one.
	 *
	 * The detached table is not.  GUP-fast and RCU pte walks that read the
	 * old PMD before pmdp_collapse_flush() may still be inside it, and on
	 * broadcast-TLBI arches that flush expels nobody.  Quiescing it would
	 * take an IPI (tlb_remove_table_sync_one()), which has nowhere to go
	 * here: outside the pmd lock it opens a pmd_none window a fault can fill,
	 * inside it is a broadcast under a spinlock.  So it goes to
	 * pte_free_defer(), which holds the free until those walkers finish, as
	 * retract_page_tables() does.  One transient table page per PMD collapse
	 * is what that costs.
	 */
	pgtable_trans_huge_deposit(mm, pmd, cand->deposit);
	map_anon_folio_pmd_nopf(cand->new_folio, pmd, vma, cand->addr);

	/* Slots with no source gain anon memory that no zap accounted */
	if (nr_populated)
		add_mm_counter(mm, MM_ANONPAGES, nr_populated);
	cand->deposit = NULL;
	cand->new_folio = NULL;	/* ownership: the mapping */
	cand->state = CAND_INSTALLED;

out_unlock:
	if (pte_ptl != pmd_ptl)
		spin_unlock(pte_ptl);
	pte_unmap(pte);
	spin_unlock(pmd_ptl);

	/* The deposit balanced the detached table, so the count is already right */
	if (old_table)
		pte_free_defer(mm, old_table);
}

/* Publish each destination folio in place of the sources it replaces */
static void collapse_install(struct vm_area_struct *vma,
			     struct collapse_control *cc, pmd_t *pmd)
{
	struct mm_struct *mm = vma->vm_mm;
	pte_t *pte, *table;
	spinlock_t *ptl;
	unsigned int i;

	if (is_pmd_order(cc->candidates[0].order)) {
		/* A PMD candidate fills the slot pool: always alone */
		VM_WARN_ON_ONCE(cc->nr_candidates != 1);
		collapse_install_pmd(vma, cc, pmd);
		return;
	}

	pte = pte_offset_map_lock(mm, pmd, cc->candidates[0].addr, &ptl);
	if (!pte) {
		/*
		 * Table gone under us (see collapse_abort_candidate() on @pte).
		 * Tear down every frozen candidate -- stranding them would leak
		 * frozen, locked sources.
		 */
		for (i = 0; i < cc->nr_candidates; i++) {
			struct collapse_candidate *cand = &cc->candidates[i];

			if (cand->state != CAND_FROZEN)
				continue;

			cand->result = SCAN_NO_PTE_TABLE;
			collapse_trace_candidate(mm, cand,
						 COLLAPSE_PASS_INSTALL);
			collapse_abort_candidate(vma, cand, NULL);
		}
		return;
	}
	table = pte - pte_index(cc->candidates[0].addr);

	for (i = 0; i < cc->nr_candidates; i++) {
		struct collapse_candidate *cand = &cc->candidates[i];
		pte_t *cand_pte = table + pte_index(cand->addr);
		unsigned int nr_populated;

		if (cand->state != CAND_FROZEN)
			continue;

		if (cand->result != SCAN_SUCCEED) {
			/* Machine check during the copy */
			collapse_abort_candidate(vma, cand, cand_pte);
			continue;
		}

		/* No destination: the provision pass could not spare one */
		if (!cand->new_folio) {
			collapse_abort_candidate(vma, cand, cand_pte);
			continue;
		}

		if (!collapse_verify_candidate(cand, cand_pte, &nr_populated)) {
			cand->result = SCAN_PTE_NON_PRESENT;
			collapse_trace_candidate(mm, cand,
						 COLLAPSE_PASS_INSTALL);
			collapse_abort_candidate(vma, cand, cand_pte);
			continue;
		}

		/*
		 * The smp_wmb() in __folio_mark_uptodate() orders the copied
		 * data before the set_ptes() that publishes it.
		 */
		__folio_mark_uptodate(cand->new_folio);
		map_anon_folio_pte_nopf(cand->new_folio, cand_pte, vma,
					cand->addr, /*uffd_wp=*/ false);

		/* Slots with no source gain anon memory that no zap accounted */
		if (nr_populated)
			add_mm_counter(mm, MM_ANONPAGES, nr_populated);
		cand->new_folio = NULL;	/* ownership: the mappings */
		cand->state = CAND_INSTALLED;
	}

	pte_unmap_unlock(pte, ptl);
}

/*
 * Lower the barriers the freeze raised, on the sources of an installed candidate
 * and on those of one that got no further.
 */
static void collapse_putback(struct vm_area_struct *vma,
			     struct collapse_control *cc)
{
	unsigned int i;

	for (i = 0; i < cc->nr_candidates; i++) {
		struct collapse_candidate *cand = &cc->candidates[i];
		const unsigned int nr_pages = candidate_nr_pages(cand);
		unsigned int k = 0;

		if (cand->state != CAND_INSTALLED)
			continue;

		while (k < nr_pages) {
			struct folio *folio;
			unsigned int nr;

			/* A slot with no source has nothing to put back */
			if (pte_none_or_zero(cand->saved_ptes[k])) {
				k++;
				continue;
			}

			folio = pte_folio(cand->saved_ptes[k]);
			nr = collapse_saved_span_len(cand, k, nr_pages);

			/*
			 * Unfreeze before the rmap drop: rmap removal munlocks
			 * under VM_LOCKED, and munlock_folio() takes a reference
			 * a frozen folio forbids.  The expected count still
			 * holds the span's mapping references; once the rmap is
			 * gone they are ours to drop, so every
			 * folio_remove_rmap_ptes() is paired with a
			 * folio_put_refs() for the same slots.  The folio lock
			 * is held until the wake below, so lock-taking rmap
			 * walkers stay excluded, and the stale-rmap window this
			 * leaves -- live folio, no PTEs -- is one any teardown of
			 * a mapped folio passes through.
			 */
			folio_ref_unfreeze(folio,
					   folio_expected_ref_count(folio) + 1);
			folio_remove_rmap_ptes(folio,
					       pte_page(cand->saved_ptes[k]),
					       nr, vma);
			folio_unlock(folio);

			/* The copy replaced it; drop the stale swap entry */
			free_swap_cache(folio);
			folio_put_refs(folio, nr + 1);

			k += nr;
		}
	}
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
	unsigned int i, nr_installed = 0;

	for (i = 0; i < cc->nr_candidates; i++) {
		struct collapse_candidate *cand = &cc->candidates[i];

		/* Never froze: the round gave up before it got that far */
		if (cand->state == CAND_SELECTED) {
			cand->state = CAND_SKIPPED;
			cand->result = result;
		}

		if (cand->new_folio) {
			folio_put(cand->new_folio);
			cand->new_folio = NULL;
		}
		if (cand->deposit) {
			pte_free(mm, cand->deposit);
			cand->deposit = NULL;
		}
		if (cand->state == CAND_INSTALLED) {
			nr_installed++;
			collapse_trace_candidate(mm, cand,
						 COLLAPSE_PASS_INSTALL);
		}
	}

	return nr_installed;
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

	cc->scan_unmapped = unmapped;
	return result;
}

/* Everything a table is judged on starts empty for each table */
static void collapse_anon_scan_init(struct collapse_control *cc)
{
	bitmap_zero(cc->eligible_ptes, MAX_PTRS_PER_PTE);
	memset(cc->node_load, 0, sizeof(cc->node_load));
	nodes_clear(cc->alloc_nmask);

	cc->select_orders = 0;
	cc->scan_unmapped = 0;
	cc->nr_collapsed = 0;
	cc->select_result = SCAN_FAIL;
	cc->smallest_alloc_failed = false;
	cc->nr_retries = 0;
}

/*
 * Judge one table's worth of @vma, leaving in @cc what a collapse could use:
 * which orders are still worth attempting, and why the table was turned down if
 * some order was.  Holds mmap_lock throughout -- it only reads -- and a caller
 * that acts on what it found hands the range to collapse_anon_pmd() afterwards,
 * without the lock.
 */
enum scan_result collapse_scan_anon_pmd(struct vm_area_struct *vma,
					unsigned long start, unsigned long end,
					struct collapse_control *cc)
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
 * Selection cuts the table into candidate windows and feeds them to rounds.  A
 * window is cut at the largest enabled order that fits and qualifies -- the PMD
 * order, when the whole table qualified -- and a region that does not qualify is
 * probed at the next enabled order below, which need not be half of it: a sparse
 * set of enabled sizes may skip several.
 *
 * Only cc->eligible_ptes is read, so a clear bit is either a hole or a PTE the
 * scan disqualified: a window's occupancy is what a collapse could use, not what
 * is present.
 */

/*
 * Largest order a window may be rooted at: the largest enabled one.
 * select_orders is fixed for the table, and the caller checked it is not empty,
 * so this is well-defined for the whole walk.
 */
static unsigned int collapse_root_order(struct collapse_control *cc)
{
	return __fls(cc->select_orders);
}

/*
 * The next enabled order below @order, or 0 when there is none.  select_orders
 * never carries an order below COLLAPSE_MIN_MTHP_ORDER -- THP_ORDERS_ALL_ANON
 * masks orders 0 and 1 -- so __fls() honours that floor by itself.  Order 0 has
 * no bits below it to mask and has to answer 0 outright: a walk that ascended
 * instead would emit a window at an offset it is not aligned for.
 */
static unsigned int collapse_lower_order(struct collapse_control *cc,
					 unsigned int order)
{
	unsigned long lower;

	if (!order)
		return 0;

	lower = cc->select_orders & GENMASK(order - 1, 0);
	return lower ? __fls(lower) : 0;
}

/* Point the selection cursor at [start, end) of the table, in PTE offsets */
static void collapse_selection_init(struct collapse_control *cc,
				    unsigned int start, unsigned int end)
{
	cc->select_start = start;
	cc->select_end = end;
	cc->select_offset = start;
	cc->select_order = min(max_order_from_offset(start),
			       collapse_root_order(cc));
}

/*
 * Advance past the region [select_offset, select_offset + nr_ptes) and determine
 * the highest order that can be attempted next.  Since huge pages must be
 * naturally aligned, it is limited by the alignment of the new offset: after an
 * order-2 mTHP at offset 0 the offset becomes 4, and __ffs(4) == 2, so the next
 * attempt starts at order 2.
 */
static void collapse_selection_advance(struct collapse_control *cc,
				       unsigned int nr_ptes)
{
	cc->select_offset += nr_ptes;
	cc->select_order = min(max_order_from_offset(cc->select_offset),
			       collapse_root_order(cc));
}

/*
 * The window at the cursor did not qualify.  Drop to the next smaller enabled
 * order over the same region, or -- when no smaller order remains -- give the
 * region up and advance the cursor past it.
 */
static void collapse_selection_reject(struct collapse_control *cc)
{
	unsigned int lower = collapse_lower_order(cc, cc->select_order);

	if (lower)
		cc->select_order = lower;
	else
		collapse_selection_advance(cc, 1U << cc->select_order);
}

/* Is the window at @offset one a collapse of @order should be attempted on? */
static bool collapse_window_eligible(struct collapse_control *cc,
				     unsigned int offset, unsigned int order)
{
	unsigned int nr_ptes = 1U << order;
	unsigned int max_ptes_none, nr_eligible_ptes;

	if (!test_bit(order, &cc->select_orders))
		return false;

	/* The window must lie inside the scanned range */
	if (offset < cc->select_start || offset + nr_ptes > cc->select_end)
		return false;

	max_ptes_none = collapse_max_ptes_none(cc, NULL, order);
	nr_eligible_ptes = bitmap_weight_from(cc->eligible_ptes, offset,
					      offset + nr_ptes);

	/*
	 * Swap PTEs the scan accepted are counted in cc->scan_unmapped, not in
	 * the bitmap.  collapse_faultin() reads them in for a PMD candidate, so
	 * there they do become sources; a smaller window leaves them as holes,
	 * sub-PMD collapse not faulting swap in.
	 */
	if (is_pmd_order(order))
		nr_eligible_ptes += cc->scan_unmapped;

	return nr_eligible_ptes >= nr_ptes - max_ptes_none;
}

/*
 * Queue the region [@offset, @end) to re-enter selection at @order.  Two
 * producers push, both in collapse_classify_result(): a refused region, tiled at
 * the next enabled order down because selection cannot tell which slot refused;
 * and a region whose in-window allocation missed, at an unchanged order, asking
 * for reclaim next time.
 *
 * The store is a stack, and the classify loop that feeds it walks the batch by
 * ascending address, so entries pop in the order they were refused rather than
 * by address: a round drawn from two of them descends.  Nothing may take
 * candidates[0] for the lowest -- what a round spans is cc->batch_start and
 * cc->batch_end, taken over its candidates by collapse_revalidate().
 *
 * Selection terminates because the tiling producer strictly descends, and the
 * unchanged-order one cannot fire twice for a region: its retry arrives with
 * reclaim set, so the next miss is a failure that descends.
 *
 * The store is sized for the most regions a table can hold, so this cannot
 * overflow; losing an entry would cost a region its lower-order attempt, so it
 * asserts rather than fails.
 */
static void collapse_push_retry(struct collapse_control *cc, unsigned int offset,
				unsigned int end, unsigned int order,
				bool reclaim)
{
	struct collapse_retry *retry;

	if (cc->nr_retries >= COLLAPSE_RETRY_STORE_SIZE) {
		VM_WARN_ON_ONCE(1);
		return;
	}

	retry = &cc->retries[cc->nr_retries];

	retry->offset = offset;
	retry->end = end;
	retry->order = order;
	retry->reclaim = reclaim;

	cc->nr_retries++;
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
				    unsigned int *offset, unsigned int *order,
				    bool *reclaim)
{
	while (cc->nr_retries) {
		struct collapse_retry *r = &cc->retries[cc->nr_retries - 1];
		unsigned int try, smallest;

		if (r->offset >= r->end) {
			cc->nr_retries--;
			continue;
		}

		/*
		 * The same walk as the table's own: the largest order the
		 * offset's alignment allows, capped by the region's, descending
		 * through the enabled orders until one fits.  If nothing fits
		 * here, step over the smallest window tried and carry on --
		 * which is what keeps the region's tail in play.
		 */
		try = min(max_order_from_offset(r->offset), r->order);
		smallest = try;
		while (try && !collapse_window_eligible(cc, r->offset, try)) {
			smallest = try;
			try = collapse_lower_order(cc, try);
		}

		if (try) {
			*offset = r->offset;
			*order = try;
			*reclaim = r->reclaim;
			r->offset += 1U << try;
			return true;
		}
		r->offset += 1U << smallest;
	}

	while (cc->select_offset < cc->select_end) {
		if (!collapse_window_eligible(cc, cc->select_offset,
					      cc->select_order)) {
			collapse_selection_reject(cc);
			continue;
		}

		/*
		 * The cursor advances past the window at emission: a round is
		 * collected before it is run, so within a round every attempt is
		 * assumed to succeed.
		 */
		*offset = cc->select_offset;
		*order = cc->select_order;
		*reclaim = false;
		collapse_selection_advance(cc, 1U << cc->select_order);
		return true;
	}

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
	unsigned int lower;

	switch (result) {
	/* Done with the region: the cursor moved past it at emission */
	case SCAN_SUCCEED:
		cc->nr_collapsed += 1U << order;
		fallthrough;
	case SCAN_PTE_MAPPED_HUGEPAGE:
		return true;
	/* Only the light allocation missed: the same order, allowed to reclaim */
	case SCAN_ALLOC_LIGHT_MISS:
		collapse_push_retry(cc, offset, offset + (1U << order), order,
				    /*reclaim=*/ true);
		return true;
	/* A smaller order over the same region might still fit */
	case SCAN_ALLOC_HUGE_PAGE_FAIL:
		/*
		 * Only a failure with nothing left below it says the allocator
		 * cannot serve this collapse.  A failure at a large order says
		 * nothing about what the region will settle for -- one PMD is
		 * 512M with 64K pages, so that attempt fails as a matter of
		 * course -- and the caller answers an allocation failure by
		 * backing off for a while.
		 */
		if (!collapse_lower_order(cc, order))
			cc->smallest_alloc_failed = true;
		fallthrough;
	case SCAN_LACK_REFERENCED_PAGE:
	case SCAN_EXCEED_NONE_PTE:
	case SCAN_EXCEED_SWAP_PTE:
	case SCAN_EXCEED_SHARED_PTE:
	case SCAN_PAGE_LOCK:
	case SCAN_PAGE_COUNT:
	case SCAN_PAGE_NOT_EXCLUSIVE:
	case SCAN_PAGE_NULL:
	case SCAN_DEL_PAGE_LRU:
	case SCAN_PTE_NON_PRESENT:
	case SCAN_PTE_UFFD:
	case SCAN_PAGE_LAZYFREE:
	case SCAN_PAGE_DIRTY_OR_WRITEBACK:
		cc->select_result = result;
		lower = collapse_lower_order(cc, order);
		if (lower) {
			/* The whole failed region re-enters, as one entry */
			collapse_push_retry(cc, offset, offset + (1U << order),
					    lower, /*reclaim=*/ false);
		}
		return true;
	/*
	 * Nothing further is worth attempting in this table.  A dropped lock
	 * belongs here rather than above: it says nothing about any window, so
	 * lowering the order of every candidate the round was carrying would be
	 * a verdict nobody reached.  The next scan finds the table again.
	 */
	case SCAN_LOCK_DROPPED:
	case SCAN_PMD_MAPPED:
	default:
		cc->select_result = result;
		cc->select_offset = cc->select_end;
		cc->nr_retries = 0;
		return false;
	}
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
static bool collapse_batch_full(struct collapse_control *cc, unsigned int slots,
				unsigned long bytes, unsigned int order)
{
	if (!cc->nr_candidates)
		return false;

	return cc->nr_candidates == COLLAPSE_MAX_CANDIDATES ||
	       slots + (1U << order) > COLLAPSE_SAVED_PTES ||
	       bytes + (PAGE_SIZE << order) > COLLAPSE_BATCH_BYTES;
}

/*
 * Take the next array slot for the window at @addr.  A slot may still hold a
 * previous round's values, so every field is set here.
 */
static void collapse_add_candidate(struct collapse_control *cc,
				   unsigned long addr, unsigned int order,
				   bool reclaim, pte_t *saved_ptes)
{
	struct collapse_candidate *cand;

	/* collapse_batch_full() has already made room */
	if (WARN_ON_ONCE(cc->nr_candidates >= COLLAPSE_MAX_CANDIDATES))
		return;

	cand = &cc->candidates[cc->nr_candidates];
	cc->nr_candidates++;
	cand->addr = addr;
	cand->order = order;
	cand->reclaim = reclaim;
	cand->state = CAND_SELECTED;
	cand->result = SCAN_FAIL;
	cand->new_folio = NULL;
	cand->deposit = NULL;
	cand->saved_ptes = saved_ptes;
}

/*
 * Cut the table into candidate windows and collapse what fits, from the
 * largest order downwards.  Returns what the table yielded: a collapse, or
 * the reason it did not.
 */
enum scan_result collapse_anon_pmd(struct mm_struct *mm, unsigned long start,
				   unsigned long end,
				   struct collapse_control *cc)
{
	const unsigned long pmd_addr = start & HPAGE_PMD_MASK;
	unsigned int offset, order;
	unsigned long bytes = 0;
	unsigned int slots = 0;
	bool pending = false, reclaim = false;
	bool cont = true;

	collapse_selection_init(cc, (start - pmd_addr) >> PAGE_SHIFT,
				(end - pmd_addr) >> PAGE_SHIFT);

	while (cont) {
		if (!pending)
			pending = collapse_next_candidate(cc, &offset, &order,
							  &reclaim);

		if (!pending || collapse_batch_full(cc, slots, bytes, order)) {
			/*
			 * Selection is exhausted and the round is empty: the
			 * range is done.  Without this a flush of an empty
			 * round would return, collect nothing, and come
			 * straight back here.
			 */
			if (!cc->nr_candidates)
				break;

			cont = collapse_run_batch(mm, pmd_addr, cc);
			slots = 0;
			bytes = 0;
			continue;
		}

		/*
		 * The round holds no resources until it is run, so
		 * collecting costs nothing but the array slot.  A candidate the
		 * full round could not take is kept pending for the next one.
		 */
		collapse_add_candidate(cc, pmd_addr + offset * PAGE_SIZE, order,
				       reclaim, cc->saved_ptes + slots);

		slots += 1U << order;
		bytes += PAGE_SIZE << order;
		pending = false;
	}

	if (cc->nr_collapsed)
		return SCAN_SUCCEED;
	/*
	 * Report an allocation failure over any refusal, the scan's included: it
	 * is the one outcome the caller acts on, by backing off rather than
	 * scanning on.
	 */
	if (cc->smallest_alloc_failed)
		return SCAN_ALLOC_HUGE_PAGE_FAIL;
	/* Nothing salvaged and nothing to wait for: say what was refused */
	if (cc->scan_refusal != SCAN_SUCCEED)
		return cc->scan_refusal;
	return cc->select_result;
}
