/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DAMON SPE (Statistical Profiling Extension) feedback
 *
 * Provides sub-THP access heatmap for intelligent split decisions.
 *
 * Three-phase architecture:
 *   Phase 2a: PTE access bits via folio_walk (current fallback)
 *   Phase 2b: Userspace SPE daemon feeds {pfn, subpage} via debugfs
 *   Phase 2c: Kernel perf_event_create_kernel_counter for ARM SPE
 *
 * Copyright (C) 2026 Wang Lian <lianux.mm@gmail.com>
 */

#ifndef _DAMON_SPE_H
#define _DAMON_SPE_H

#include <linux/mm_types.h>
#include <linux/types.h>

#ifdef CONFIG_DAMON_SPE

/* ---- Sub-page heatmap query ---- */

int damon_spe_folio_heatmap(struct folio *folio, struct vm_area_struct *vma,
			    unsigned long addr, unsigned int target_order,
			    unsigned long *hot_bitmap);

int damon_spe_hot_fraction(struct folio *folio, struct vm_area_struct *vma,
			   unsigned long addr, unsigned int target_order);

/* ---- Recording (called from SPE event handler or userspace daemon) ---- */

void damon_spe_record_access(unsigned long pfn, pid_t pid);

/* ---- Maintenance ---- */

void damon_spe_prune(void);
void damon_spe_stats(unsigned int *nr_entries, unsigned long *total_accesses);

#else /* !CONFIG_DAMON_SPE */

static inline int damon_spe_folio_heatmap(struct folio *folio,
		struct vm_area_struct *vma, unsigned long addr,
		unsigned int target_order, unsigned long *hot_bitmap)
{
	return -EOPNOTSUPP;
}

static inline int damon_spe_hot_fraction(struct folio *folio,
		struct vm_area_struct *vma, unsigned long addr,
		unsigned int target_order)
{
	return -EOPNOTSUPP;
}

static inline void damon_spe_record_access(unsigned long pfn, pid_t pid) {}
static inline void damon_spe_prune(void) {}
static inline void damon_spe_stats(unsigned int *nr, unsigned long *total) {}

#endif /* CONFIG_DAMON_SPE */
#endif /* _DAMON_SPE_H */
