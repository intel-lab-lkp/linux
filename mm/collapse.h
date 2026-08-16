/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MM_COLLAPSE_H
#define __MM_COLLAPSE_H

#include <linux/mm.h>
#include <linux/nodemask.h>
#include <linux/pgtable.h>
#include <linux/types.h>

/* The smallest order a collapse will build, and so the finest window it cuts */
#define COLLAPSE_MIN_MTHP_ORDER		2

struct collapse_candidate;

enum scan_result {
	SCAN_FAIL,
	SCAN_SUCCEED,
	SCAN_NO_PTE_TABLE,
	SCAN_PMD_MAPPED,
	SCAN_EXCEED_NONE_PTE,
	SCAN_EXCEED_SWAP_PTE,
	SCAN_EXCEED_SHARED_PTE,
	SCAN_PTE_NON_PRESENT,
	SCAN_PTE_UFFD,
	SCAN_PTE_MAPPED_HUGEPAGE,
	SCAN_LACK_REFERENCED_PAGE,
	SCAN_PAGE_NULL,
	SCAN_SCAN_ABORT,
	SCAN_PAGE_COUNT,
	SCAN_PAGE_LRU,
	SCAN_PAGE_LOCK,
	SCAN_LOCK_DROPPED,
	SCAN_PAGE_ANON,
	SCAN_PAGE_LAZYFREE,
	SCAN_PAGE_COMPOUND,
	SCAN_ANY_PROCESS,
	SCAN_VMA_NULL,
	SCAN_VMA_CHECK,
	SCAN_ADDRESS_RANGE,
	SCAN_DEL_PAGE_LRU,
	SCAN_ALLOC_HUGE_PAGE_FAIL,
	SCAN_CGROUP_CHARGE_FAIL,
	SCAN_TRUNCATED,
	SCAN_PAGE_HAS_PRIVATE,
	SCAN_STORE_FAILED,
	SCAN_COPY_MC,
	SCAN_PAGE_FILLED,
	SCAN_PAGE_DIRTY_OR_WRITEBACK,
	SCAN_ALLOC_LIGHT_MISS,
};

/*
 * What a collapse is allowed to do, decided by whoever asked for it, so the
 * code doing it need not ask who its caller is: khugepaged fills this in from
 * its own settings, MADV_COLLAPSE from the fact that a user asked explicitly.
 */
struct collapse_policy {
	/* Limits, stated per PMD; HPAGE_PMD_NR means "no limit" */
	unsigned int max_ptes_none;
	unsigned int max_ptes_swap;
	unsigned int max_ptes_shared;

	/*
	 * Hold a sub-PMD window to a stricter rule than a PMD: no swapped-out
	 * and no shared PTEs at all, and max_ptes_none as
	 * collapse_max_ptes_none() scales it.  khugepaged holds mTHP collapse
	 * to that; an explicit request does not.
	 */
	bool strict_sub_pmd;

	/*
	 * Collapse only where it looks worth doing: require some sign the range
	 * is in use, and leave clean lazyfree folios for reclaim rather than
	 * collapsing them into a folio that is not lazyfree.  A user who asked
	 * for a collapse gets one either way.
	 */
	bool skip_lazyfree;
	bool require_referenced;

	/*
	 * Finish the job rather than leaving it half done for a fault to pick
	 * up: map the PMD over a file collapse before returning, and write
	 * dirty pages back and retry once instead of refusing them.  Both cost
	 * latency the caller has asked to pay.
	 */
	bool install_pmd;
	bool writeback_dirty;

	/* How hard to try for a destination folio */
	gfp_t gfp;

	/* Which VMAs are eligible, as thp_vma_allowable_orders() spells it */
	enum tva_type tva_type;
};

struct collapse_control {
	struct collapse_policy policy;

	bool is_khugepaged;

	/* Num pages scanned per node */
	u32 node_load[MAX_NUMNODES];

	/* Num pages scanned (see khugepaged_pages_to_scan) */
	unsigned int progress;

	/* nodemask for allocation fallback */
	nodemask_t alloc_nmask;

	/* Each bit marks a PTE the scan accepted as a collapse source */
	DECLARE_BITMAP(eligible_ptes, MAX_PTRS_PER_PTE);

	/* Orders still worth attempting in the table being scanned */
	unsigned long select_orders;

	/* PTEs collapsed in it so far */
	unsigned int nr_collapsed;

	/*
	 * Why the scan would not take all of the table, or SCAN_SUCCEED if it
	 * took every order it was offered.  Not the opposite of what the scan
	 * selected: a table can be worth collapsing at one order and refused at
	 * another, so a scan that found work still has a reason to report, and
	 * the collapse reports it when it salvages nothing.
	 */
	enum scan_result scan_refusal;

	/* The candidate windows collected for the current round */
	struct collapse_candidate *candidates;
	unsigned int nr_candidates;

	/*
	 * What the candidates the round still means to freeze span, settled by
	 * collapse_revalidate() as it walks them.  A round is not necessarily
	 * address-ordered, so this cannot be read off the ends of the array.
	 */
	unsigned long batch_start;
	unsigned long batch_end;
};

int collapse_control_init(struct collapse_control *cc);
void collapse_control_release(struct collapse_control *cc);

/*
 * Defined in khugepaged.c, which still uses them itself.
 * TODO: move each into collapse.c once its last khugepaged.c user is gone.
 */
unsigned long collapse_possible_orders(struct vm_area_struct *vma,
		vm_flags_t vm_flags, enum tva_type tva_flags);
enum scan_result find_pmd_or_thp_or_none(struct mm_struct *mm,
		unsigned long address, pmd_t **pmd);
int collapse_find_target_node(struct collapse_control *cc);
bool collapse_scan_abort(int nid, struct collapse_control *cc);
unsigned int collapse_max_ptes_none(struct collapse_control *cc,
		struct vm_area_struct *vma, unsigned int order);
unsigned int collapse_max_ptes_swap(struct collapse_control *cc,
		unsigned int order);
unsigned int collapse_max_ptes_shared(struct collapse_control *cc,
		unsigned int order);

#endif	/* __MM_COLLAPSE_H */
