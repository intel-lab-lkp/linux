/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MM_COLLAPSE_H
#define __MM_COLLAPSE_H

#include <linux/mm.h>
#include <linux/nodemask.h>
#include <linux/pgtable.h>
#include <linux/types.h>

/* The most the max_ptes_* tunables accept */
#define COLLAPSE_MAX_PTES_LIMIT		(HPAGE_PMD_NR - 1)

/* The smallest order a collapse will build */
#define COLLAPSE_MIN_MTHP_ORDER		2

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
};

/* What a collapse is allowed to do, decided by the caller that asks for it */
struct collapse_policy {
	/* Limits, stated per PMD; HPAGE_PMD_NR means "no limit" */
	unsigned int max_ptes_none;
	unsigned int max_ptes_swap;
	unsigned int max_ptes_shared;

	/*
	 * Hold a sub-PMD window to a stricter rule than a PMD: no swapped-out
	 * and no shared PTEs at all, and max_ptes_none as
	 * collapse_max_ptes_none() scales it.
	 */
	bool strict_sub_pmd;

	/*
	 * Collapse only where it looks worth doing: require some sign the
	 * range is in use, and leave clean lazyfree folios for reclaim rather
	 * than collapsing them into a folio that is not lazyfree.
	 */
	bool skip_lazyfree;
	bool require_referenced;

	/*
	 * Finish the job rather than leaving it half done for a fault to pick
	 * up: map the PMD over a file collapse before returning, and write
	 * dirty pages back and retry once instead of refusing them.  Both cost
	 * latency the caller has to be willing to pay.
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

	/* Num pages scanned per node */
	u32 node_load[MAX_NUMNODES];

	/* Num pages scanned (see khugepaged_pages_to_scan) */
	unsigned int progress;

	/* nodemask for allocation fallback */
	nodemask_t alloc_nmask;

	/* Each bit marks a PTE the scan accepted as a collapse source */
	DECLARE_BITMAP(eligible_ptes, MAX_PTRS_PER_PTE);

	/*
	 * What a scan found and the run after it needs.  Live only between the
	 * two, and read by nobody else.
	 *
	 * The file side takes a reference while it still has the VMA, since a
	 * file collapse works on the page cache and never sees one; the run is
	 * what gives it back.
	 */
	unsigned long scan_orders;
	int scan_referenced;
	int scan_unmapped;
	struct file *scan_file;
	pgoff_t scan_pgoff;
};

#endif	/* __MM_COLLAPSE_H */
