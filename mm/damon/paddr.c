// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON Primitives for The Physical Address Space
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#define pr_fmt(fmt) "damon-pa: " fmt

#include <linux/mmu_notifier.h>
#include <linux/page_idle.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/memory-tiers.h>
#include <linux/migrate.h>
#include <linux/mm_inline.h>

#include "../internal.h"
#include "ops-common.h"

static bool __damon_pa_mkold(struct folio *folio, struct vm_area_struct *vma,
		unsigned long addr, void *arg)
{
	DEFINE_FOLIO_VMA_WALK(pvmw, folio, vma, addr, 0);

	while (page_vma_mapped_walk(&pvmw)) {
		addr = pvmw.address;
		if (pvmw.pte)
			damon_ptep_mkold(pvmw.pte, vma, addr);
		else
			damon_pmdp_mkold(pvmw.pmd, vma, addr);
	}
	return true;
}

static void damon_pa_mkold(unsigned long paddr)
{
	struct folio *folio = damon_get_folio(PHYS_PFN(paddr));
	struct rmap_walk_control rwc = {
		.rmap_one = __damon_pa_mkold,
		.anon_lock = folio_lock_anon_vma_read,
	};
	bool need_lock;

	if (!folio)
		return;

	if (!folio_mapped(folio) || !folio_raw_mapping(folio)) {
		folio_set_idle(folio);
		goto out;
	}

	need_lock = !folio_test_anon(folio) || folio_test_ksm(folio);
	if (need_lock && !folio_trylock(folio))
		goto out;

	rmap_walk(folio, &rwc);

	if (need_lock)
		folio_unlock(folio);

out:
	folio_put(folio);
}

static void __damon_pa_prepare_access_check(struct damon_region *r)
{
	r->sampling_addr = damon_rand(r->ar.start, r->ar.end);

	damon_pa_mkold(r->sampling_addr);
}

static void damon_pa_prepare_access_checks(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t)
			__damon_pa_prepare_access_check(r);
	}
}

static bool __damon_pa_young(struct folio *folio, struct vm_area_struct *vma,
		unsigned long addr, void *arg)
{
	bool *accessed = arg;
	DEFINE_FOLIO_VMA_WALK(pvmw, folio, vma, addr, 0);

	*accessed = false;
	while (page_vma_mapped_walk(&pvmw)) {
		addr = pvmw.address;
		if (pvmw.pte) {
			*accessed = pte_young(ptep_get(pvmw.pte)) ||
				!folio_test_idle(folio) ||
				mmu_notifier_test_young(vma->vm_mm, addr);
		} else {
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
			*accessed = pmd_young(pmdp_get(pvmw.pmd)) ||
				!folio_test_idle(folio) ||
				mmu_notifier_test_young(vma->vm_mm, addr);
#else
			WARN_ON_ONCE(1);
#endif	/* CONFIG_TRANSPARENT_HUGEPAGE */
		}
		if (*accessed) {
			page_vma_mapped_walk_done(&pvmw);
			break;
		}
	}

	/* If accessed, stop walking */
	return *accessed == false;
}

static bool damon_pa_young(unsigned long paddr, unsigned long *folio_sz)
{
	struct folio *folio = damon_get_folio(PHYS_PFN(paddr));
	bool accessed = false;
	struct rmap_walk_control rwc = {
		.arg = &accessed,
		.rmap_one = __damon_pa_young,
		.anon_lock = folio_lock_anon_vma_read,
	};
	bool need_lock;

	if (!folio)
		return false;

	if (!folio_mapped(folio) || !folio_raw_mapping(folio)) {
		if (folio_test_idle(folio))
			accessed = false;
		else
			accessed = true;
		goto out;
	}

	need_lock = !folio_test_anon(folio) || folio_test_ksm(folio);
	if (need_lock && !folio_trylock(folio))
		goto out;

	rmap_walk(folio, &rwc);

	if (need_lock)
		folio_unlock(folio);

out:
	*folio_sz = folio_size(folio);
	folio_put(folio);
	return accessed;
}

static void __damon_pa_check_access(struct damon_region *r,
		struct damon_attrs *attrs)
{
	static unsigned long last_addr;
	static unsigned long last_folio_sz = PAGE_SIZE;
	static bool last_accessed;

	/* If the region is in the last checked page, reuse the result */
	if (ALIGN_DOWN(last_addr, last_folio_sz) ==
				ALIGN_DOWN(r->sampling_addr, last_folio_sz)) {
		damon_update_region_access_rate(r, last_accessed, attrs);
		return;
	}

	last_accessed = damon_pa_young(r->sampling_addr, &last_folio_sz);
	damon_update_region_access_rate(r, last_accessed, attrs);

	last_addr = r->sampling_addr;
}

static unsigned int damon_pa_check_accesses(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;
	unsigned int max_nr_accesses = 0;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t) {
			__damon_pa_check_access(r, &ctx->attrs);
			max_nr_accesses = max(r->nr_accesses, max_nr_accesses);
		}
	}

	return max_nr_accesses;
}

static bool __damos_pa_filter_out(struct damos_filter *filter,
		struct folio *folio)
{
	bool matched = false;
	struct mem_cgroup *memcg;

	switch (filter->type) {
	case DAMOS_FILTER_TYPE_ANON:
		matched = folio_test_anon(folio);
		break;
	case DAMOS_FILTER_TYPE_MEMCG:
		rcu_read_lock();
		memcg = folio_memcg_check(folio);
		if (!memcg)
			matched = false;
		else
			matched = filter->memcg_id == mem_cgroup_id(memcg);
		rcu_read_unlock();
		break;
	default:
		break;
	}

	return matched == filter->matching;
}

/*
 * damos_pa_filter_out - Return true if the page should be filtered out.
 */
static bool damos_pa_filter_out(struct damos *scheme, struct folio *folio)
{
	struct damos_filter *filter;

	damos_for_each_filter(filter, scheme) {
		if (__damos_pa_filter_out(filter, folio))
			return true;
	}
	return false;
}

enum migration_mode {
	MIG_PAGEOUT,
	MIG_PROMOTE,
	MIG_DEMOTE,
};

/*
 * XXX: This is copied from demote_folio_list as renamed as migrate_folio_list.
 * Take folios on @migrate_folios and attempt to migrate them to another node.
 * Folios which are not migrated are left on @migrate_folios.
 */
static unsigned int migrate_folio_list(struct list_head *migrate_folios,
				       struct pglist_data *pgdat,
				       enum migration_mode mm,
				       int target_nid)
{
	unsigned int nr_succeeded;
	nodemask_t allowed_mask;
	int reason;
	enum vm_event_item vm_event;

	switch (mm) {
	case MIG_PROMOTE:
		if (target_nid == NUMA_NO_NODE)
			target_nid = next_promotion_node(pgdat->node_id);
		reason = MR_PROMOTION;
		vm_event = PGPROMOTE;
		break;
	case MIG_DEMOTE:
		if (target_nid == NUMA_NO_NODE)
			target_nid = next_demotion_node(pgdat->node_id);
		reason = MR_DEMOTION;
		vm_event = PGDEMOTE_DIRECT;
		break;
	default:
		return 0;
	}

	struct migration_target_control mtc = {
		/*
		 * Allocate from 'node', or fail quickly and quietly.
		 * When this happens, 'page' will likely just be discarded
		 * instead of migrated.
		 */
		.gfp_mask = (GFP_HIGHUSER_MOVABLE & ~__GFP_RECLAIM) | __GFP_NOWARN |
			__GFP_NOMEMALLOC | GFP_NOWAIT,
		.nid = target_nid,
		.nmask = &allowed_mask
	};

	if (pgdat->node_id == target_nid || target_nid == NUMA_NO_NODE)
		return 0;

	if (list_empty(migrate_folios))
		return 0;

	if (mm == MIG_DEMOTE) {
		node_get_allowed_targets(pgdat, &allowed_mask);
	} else if (mm == MIG_PROMOTE) {
		/* TODO: Need to add upper_tier_mask at struct memory_tier. */
		allowed_mask = NODE_MASK_NONE;
	}

	/* Migration ignores all cpuset and mempolicy settings */
	migrate_pages(migrate_folios, alloc_migrate_folio, NULL,
		      (unsigned long)&mtc, MIGRATE_ASYNC, reason,
		      &nr_succeeded);

	__count_vm_events(vm_event, nr_succeeded);

	return nr_succeeded;
}

enum folio_references {
	FOLIOREF_RECLAIM,
	FOLIOREF_KEEP,
	FOLIOREF_ACTIVATE,
};

/*
 * XXX: This is just copied and simplified from folio_check_references at
 *      mm/vmscan.c but without having scan_control.
 */
static enum folio_references folio_check_references(struct folio *folio)
{
	int referenced_ptes, referenced_folio;
	unsigned long vm_flags;

	referenced_ptes = folio_referenced(folio, 1, NULL, &vm_flags);
	referenced_folio = folio_test_clear_referenced(folio);

	/* rmap lock contention: rotate */
	if (referenced_ptes == -1)
		return FOLIOREF_KEEP;

	if (referenced_ptes) {
		/*
		 * All mapped folios start out with page table
		 * references from the instantiating fault, so we need
		 * to look twice if a mapped file/anon folio is used more
		 * than once.
		 *
		 * Mark it and spare it for another trip around the
		 * inactive list.  Another page table reference will
		 * lead to its activation.
		 *
		 * Note: the mark is set for activated folios as well
		 * so that recently deactivated but used folios are
		 * quickly recovered.
		 */
		folio_set_referenced(folio);

		if (referenced_folio || referenced_ptes > 1)
			return FOLIOREF_ACTIVATE;

		/*
		 * Activate file-backed executable folios after first usage.
		 */
		if ((vm_flags & VM_EXEC) && folio_is_file_lru(folio))
			return FOLIOREF_ACTIVATE;

		return FOLIOREF_KEEP;
	}

	return FOLIOREF_RECLAIM;
}

/*
 * XXX: This is minimized implmentation based on shrink_folio_list only for
 *      the demotion calling demote_folio_list.
 */
static unsigned int damon_pa_migrate_folio_list(struct list_head *folio_list,
						struct pglist_data *pgdat,
						enum migration_mode mm,
						int target_nid)
{
	unsigned int nr_migrated = 0;
	struct folio *folio;
	LIST_HEAD(ret_folios);
	LIST_HEAD(migrate_folios);

	cond_resched();

	while (!list_empty(folio_list)) {
		struct folio *folio;
		enum folio_references references;

		cond_resched();

		folio = lru_to_folio(folio_list);
		list_del(&folio->lru);

		if (!folio_trylock(folio))
			goto keep;

		VM_BUG_ON_FOLIO(folio_test_active(folio), folio);

		references = folio_check_references(folio);
		if (references == FOLIOREF_KEEP ||
		    (references == FOLIOREF_RECLAIM && mm == MIG_PROMOTE))
			goto keep_locked;

		/* Relocate its contents to another node. */
		list_add(&folio->lru, &migrate_folios);
		folio_unlock(folio);
		continue;
keep_locked:
		folio_unlock(folio);
keep:
		list_add(&folio->lru, &ret_folios);
		VM_BUG_ON_FOLIO(folio_test_lru(folio), folio);
	}
	/* 'folio_list' is always empty here */

	/* Migrate folios selected for migration */
	nr_migrated += migrate_folio_list(&migrate_folios, pgdat, mm, target_nid);
	/* Folios that could not be migrated are still in @migrate_folios */
	if (!list_empty(&migrate_folios)) {
		/* Folios which weren't migrated go back on @folio_list */
		list_splice_init(&migrate_folios, folio_list);
	}

	try_to_unmap_flush();

	list_splice(&ret_folios, folio_list);

	while (!list_empty(folio_list)) {
		folio = lru_to_folio(folio_list);
		list_del(&folio->lru);
		folio_putback_lru(folio);
	}

	return nr_migrated;
}

/*
 * XXX: This is almost identical to reclaim_pages() in mm/vmscan.c, but it
 *      internally calls damon_pa_migrate_folio_list() instead of
 *      reclaim_folio_list().  We might be better to think if we can have a
 *      common function for both cases.
 */
static unsigned long damon_pa_migrate_pages(struct list_head *folio_list,
					    enum migration_mode mm,
					    int target_nid)
{
	int nid;
	unsigned int nr_migrated = 0;
	LIST_HEAD(node_folio_list);
	unsigned int noreclaim_flag;

	if (list_empty(folio_list))
		return nr_migrated;

	noreclaim_flag = memalloc_noreclaim_save();

	nid = folio_nid(lru_to_folio(folio_list));
	do {
		struct folio *folio = lru_to_folio(folio_list);

		if (nid == folio_nid(folio)) {
			folio_clear_active(folio);
			list_move(&folio->lru, &node_folio_list);
			continue;
		}

		nr_migrated += damon_pa_migrate_folio_list(&node_folio_list,
							   NODE_DATA(nid), mm,
							   target_nid);
		nid = folio_nid(lru_to_folio(folio_list));
	} while (!list_empty(folio_list));

	nr_migrated += damon_pa_migrate_folio_list(&node_folio_list,
						   NODE_DATA(nid), mm,
						   target_nid);

	memalloc_noreclaim_restore(noreclaim_flag);

	return nr_migrated;
}

static unsigned long damon_pa_migrate(struct damon_region *r, struct damos *s,
				      enum migration_mode mm)
{
	unsigned long addr, applied;
	LIST_HEAD(folio_list);

	for (addr = r->ar.start; addr < r->ar.end; addr += PAGE_SIZE) {
		struct folio *folio = damon_get_folio(PHYS_PFN(addr));

		if (!folio)
			continue;

		if (damos_pa_filter_out(s, folio))
			goto put_folio;

		if (mm != MIG_PROMOTE) {
			folio_clear_referenced(folio);
			folio_test_clear_young(folio);
		}
		if (!folio_isolate_lru(folio))
			goto put_folio;
		/*
		 * Since unevictable folios can be demoted or promoted,
		 * unevictable test is needed only for pageout.
		 */
		if (mm == MIG_PAGEOUT && folio_test_unevictable(folio))
			folio_putback_lru(folio);
		else
			list_add(&folio->lru, &folio_list);
put_folio:
		folio_put(folio);
	}
	switch (mm) {
	case MIG_PAGEOUT:
		applied = reclaim_pages(&folio_list);
		break;
	case MIG_PROMOTE:
	case MIG_DEMOTE:
		applied = damon_pa_migrate_pages(&folio_list, mm,
						 s->target_nid);
		break;
	default:
		/* Unexpected migration mode. */
		return 0;
	}
	cond_resched();
	return applied * PAGE_SIZE;
}

static inline unsigned long damon_pa_mark_accessed_or_deactivate(
		struct damon_region *r, struct damos *s, bool mark_accessed)
{
	unsigned long addr, applied = 0;

	for (addr = r->ar.start; addr < r->ar.end; addr += PAGE_SIZE) {
		struct folio *folio = damon_get_folio(PHYS_PFN(addr));

		if (!folio)
			continue;

		if (damos_pa_filter_out(s, folio))
			goto put_folio;

		if (mark_accessed)
			folio_mark_accessed(folio);
		else
			folio_deactivate(folio);
		applied += folio_nr_pages(folio);
put_folio:
		folio_put(folio);
	}
	return applied * PAGE_SIZE;
}

static unsigned long damon_pa_mark_accessed(struct damon_region *r,
	struct damos *s)
{
	return damon_pa_mark_accessed_or_deactivate(r, s, true);
}

static unsigned long damon_pa_deactivate_pages(struct damon_region *r,
	struct damos *s)
{
	return damon_pa_mark_accessed_or_deactivate(r, s, false);
}

static unsigned long damon_pa_apply_scheme(struct damon_ctx *ctx,
		struct damon_target *t, struct damon_region *r,
		struct damos *scheme)
{
	switch (scheme->action) {
	case DAMOS_PAGEOUT:
		return damon_pa_migrate(r, scheme, MIG_PAGEOUT);
	case DAMOS_LRU_PRIO:
		return damon_pa_mark_accessed(r, scheme);
	case DAMOS_LRU_DEPRIO:
		return damon_pa_deactivate_pages(r, scheme);
	case DAMOS_PROMOTE:
		return damon_pa_migrate(r, scheme, MIG_PROMOTE);
	case DAMOS_DEMOTE:
		return damon_pa_migrate(r, scheme, MIG_DEMOTE);
	case DAMOS_STAT:
		break;
	default:
		/* DAMOS actions that not yet supported by 'paddr'. */
		break;
	}
	return 0;
}

static int damon_pa_scheme_score(struct damon_ctx *context,
		struct damon_target *t, struct damon_region *r,
		struct damos *scheme)
{
	switch (scheme->action) {
	case DAMOS_PAGEOUT:
		return damon_cold_score(context, r, scheme);
	case DAMOS_LRU_PRIO:
		return damon_hot_score(context, r, scheme);
	case DAMOS_LRU_DEPRIO:
		return damon_cold_score(context, r, scheme);
	case DAMOS_PROMOTE:
		return damon_hot_score(context, r, scheme);
	case DAMOS_DEMOTE:
		return damon_cold_score(context, r, scheme);
	default:
		break;
	}

	return DAMOS_MAX_SCORE;
}

static int __init damon_pa_initcall(void)
{
	struct damon_operations ops = {
		.id = DAMON_OPS_PADDR,
		.init = NULL,
		.update = NULL,
		.prepare_access_checks = damon_pa_prepare_access_checks,
		.check_accesses = damon_pa_check_accesses,
		.reset_aggregated = NULL,
		.target_valid = NULL,
		.cleanup = NULL,
		.apply_scheme = damon_pa_apply_scheme,
		.get_scheme_score = damon_pa_scheme_score,
	};

	return damon_register_ops(&ops);
};

subsys_initcall(damon_pa_initcall);
