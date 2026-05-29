// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/swap_state.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *
 *  Rewritten to use page cache, (C) 1998 Stephen Tweedie
 */
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/mempolicy.h>
#include <linux/swap.h>
#include <linux/leafops.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/folio_batch.h>
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/migrate.h>
#include <linux/vmalloc.h>
#include <linux/huge_mm.h>
#include <linux/sizes.h>
#include <linux/zswap.h>
#include <linux/shmem_fs.h>
#include "internal.h"
#include "swap_table.h"
#include "swap.h"

/*
 * swapper_space is a fiction, retained to simplify the path through
 * vmscan's shrink_folio_list.
 */
static const struct address_space_operations swap_aops = {
	.dirty_folio	= noop_dirty_folio,
#ifdef CONFIG_MIGRATION
	.migrate_folio	= migrate_folio,
#endif
};

struct address_space swap_space __read_mostly = {
	.a_ops = &swap_aops,
};

static bool enable_vma_readahead __read_mostly = true;

#define SWAP_RA_ORDER_CEILING	5

#define SWAP_RA_WIN_SHIFT	(PAGE_SHIFT / 2)
#define SWAP_RA_HITS_MASK	((1UL << SWAP_RA_WIN_SHIFT) - 1)
#define SWAP_RA_HITS_MAX	SWAP_RA_HITS_MASK
#define SWAP_RA_WIN_MASK	(~PAGE_MASK & ~SWAP_RA_HITS_MASK)

#define SWAP_RA_HITS(v)		((v) & SWAP_RA_HITS_MASK)
#define SWAP_RA_WIN(v)		(((v) & SWAP_RA_WIN_MASK) >> SWAP_RA_WIN_SHIFT)
#define SWAP_RA_ADDR(v)		((v) & PAGE_MASK)

#define SWAP_RA_VAL(addr, win, hits)				\
	(((addr) & PAGE_MASK) |					\
	 (((win) << SWAP_RA_WIN_SHIFT) & SWAP_RA_WIN_MASK) |	\
	 ((hits) & SWAP_RA_HITS_MASK))

/* Initial readahead hits is 4 to start up with a small window */
#define GET_SWAP_RA_VAL(vma)					\
	(atomic_long_read(&(vma)->swap_readahead_info) ? : 4)

static atomic_t swapin_readahead_hits = ATOMIC_INIT(4);

void show_swap_cache_info(void)
{
	printk("%lu pages in swap cache\n", total_swapcache_pages());
	printk("Free swap  = %ldkB\n", K(get_nr_swap_pages()));
	printk("Total swap = %lukB\n", K(total_swap_pages));
}

/**
 * swap_cache_get_folio - Looks up a folio in the swap cache.
 * @entry: swap entry used for the lookup.
 *
 * A found folio will be returned unlocked and with its refcount increased.
 *
 * Context: Caller must ensure @entry is valid and protect the swap device
 * with reference count or locks.
 * Return: Returns the found folio on success, NULL otherwise. The caller
 * must lock and check if the folio still matches the swap entry before
 * use (e.g., folio_matches_swap_entry).
 */
struct folio *swap_cache_get_folio(swp_entry_t entry)
{
	unsigned long swp_tb;
	struct folio *folio;

	for (;;) {
		swp_tb = swap_table_get(__swap_entry_to_cluster(entry),
					swp_cluster_offset(entry));
		if (!swp_tb_is_folio(swp_tb))
			return NULL;
		folio = swp_tb_to_folio(swp_tb);
		if (likely(folio_try_get(folio)))
			return folio;
	}

	return NULL;
}

/**
 * swap_cache_has_folio - Check if a swap slot has cache.
 * @entry: swap entry indicating the slot.
 *
 * Context: Caller must ensure @entry is valid and protect the swap
 * device with reference count or locks.
 */
bool swap_cache_has_folio(swp_entry_t entry)
{
	unsigned long swp_tb;

	swp_tb = swap_table_get(__swap_entry_to_cluster(entry),
				swp_cluster_offset(entry));
	return swp_tb_is_folio(swp_tb);
}

/**
 * swap_cache_get_shadow - Looks up a shadow in the swap cache.
 * @entry: swap entry used for the lookup.
 *
 * Context: Caller must ensure @entry is valid and protect the swap device
 * with reference count or locks.
 * Return: Returns either NULL or an XA_VALUE (shadow).
 */
void *swap_cache_get_shadow(swp_entry_t entry)
{
	unsigned long swp_tb;

	swp_tb = swap_table_get(__swap_entry_to_cluster(entry),
				swp_cluster_offset(entry));
	if (swp_tb_is_shadow(swp_tb))
		return swp_tb_to_shadow(swp_tb);
	return NULL;
}

/**
 * __swap_cache_add_check - Check if a range is suitable for adding a folio.
 * @ci: The locked swap cluster
 * @targ_entry: The target swap entry to check, will be rounded down by @nr
 * @nr: Number of slots to check, must be a power of 2
 * @shadowp: Returns the shadow value if one exists in the range
 * @memcg_id: Returns the memory cgroup id, NULL to ignore cgroup check
 *
 * Check if all slots covered by given range have a swap count >= 1.
 * Retrieves the shadow if there is one. If @memcg_id is not NULL, also
 * checks if all slots belong to the same cgroup and return the cgroup
 * private id.
 *
 * Context: Caller must lock the cluster.
 * Return: 0 if success, error code if failed.
 */
static int __swap_cache_add_check(struct swap_cluster_info *ci,
				  swp_entry_t targ_entry,
				  unsigned long nr, void **shadowp,
				  unsigned short *memcg_id)
{
	unsigned int ci_off, ci_end;
	unsigned long old_tb;
	bool is_zero;

	lockdep_assert_held(&ci->lock);

	/*
	 * If the target slot is not swapped out or already cached, return
	 * -ENOENT or -EEXIST. If the batch is not suitable, could be a
	 * race with concurrent free or cache add, return -EBUSY.
	 */
	if (unlikely(!ci->table))
		return -ENOENT;
	ci_off = swp_cluster_offset(targ_entry);
	old_tb = __swap_table_get(ci, ci_off);
	if (swp_tb_is_folio(old_tb))
		return -EEXIST;
	if (!__swp_tb_get_count(old_tb))
		return -ENOENT;
	if (shadowp && swp_tb_is_shadow(old_tb))
		*shadowp = swp_tb_to_shadow(old_tb);
	if (memcg_id)
		*memcg_id = __swap_cgroup_get(ci, ci_off);

	if (nr == 1)
		return 0;

	is_zero = __swap_table_test_zero(ci, ci_off);
	ci_off = round_down(ci_off, nr);
	ci_end = ci_off + nr;
	do {
		old_tb = __swap_table_get(ci, ci_off);
		if (unlikely(swp_tb_is_folio(old_tb) ||
			     !__swp_tb_get_count(old_tb) ||
			     is_zero != __swap_table_test_zero(ci, ci_off) ||
			     (memcg_id && *memcg_id != __swap_cgroup_get(ci, ci_off))))
			return -EBUSY;
	} while (++ci_off < ci_end);

	return 0;
}

static void __swap_cache_do_add_folio(struct swap_cluster_info *ci,
				      struct folio *folio, swp_entry_t entry)
{
	unsigned int ci_off = swp_cluster_offset(entry), ci_end;
	unsigned long nr_pages = folio_nr_pages(folio);
	unsigned long pfn = folio_pfn(folio);
	unsigned long old_tb;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(folio_test_swapcache(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapbacked(folio), folio);

	ci_end = ci_off + nr_pages;
	do {
		old_tb = __swap_table_get(ci, ci_off);
		VM_WARN_ON_ONCE(swp_tb_is_folio(old_tb));
		__swap_table_set(ci, ci_off, pfn_to_swp_tb(pfn, __swp_tb_get_flags(old_tb)));
	} while (++ci_off < ci_end);

	folio_ref_add(folio, nr_pages);
	folio_set_swapcache(folio);
	folio->swap = entry;
}

/**
 * __swap_cache_add_folio - Add a folio to the swap cache and update stats.
 * @ci: The locked swap cluster.
 * @folio: The folio to be added.
 * @entry: The swap entry corresponding to the folio.
 *
 * Unconditionally add a folio to the swap cache. The caller must ensure
 * all slots are usable and have no conflicts. This assigns entry to
 * @folio->swap, increases folio refcount by the number of pages, and
 * updates swap cache stats.
 *
 * Context: Caller must ensure the folio is locked and lock the cluster
 * that holds the entries.
 */
void __swap_cache_add_folio(struct swap_cluster_info *ci,
			    struct folio *folio, swp_entry_t entry)
{
	unsigned long nr_pages = folio_nr_pages(folio);

	__swap_cache_do_add_folio(ci, folio, entry);
	node_stat_mod_folio(folio, NR_FILE_PAGES, nr_pages);
	lruvec_stat_mod_folio(folio, NR_SWAPCACHE, nr_pages);
}

static void __swap_cache_do_del_folio(struct swap_cluster_info *ci,
				      struct folio *folio,
				      swp_entry_t entry, void *shadow)
{
	unsigned long old_tb;
	struct swap_info_struct *si;
	unsigned int ci_start, ci_off, ci_end;
	bool folio_swapped = false, need_free = false;
	unsigned long nr_pages = folio_nr_pages(folio);

	VM_WARN_ON_ONCE(__swap_entry_to_cluster(entry) != ci);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapcache(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(folio_test_writeback(folio), folio);

	si = __swap_entry_to_info(entry);
	ci_start = swp_cluster_offset(entry);
	ci_end = ci_start + nr_pages;
	ci_off = ci_start;
	do {
		old_tb = __swap_table_get(ci, ci_off);
		WARN_ON_ONCE(!swp_tb_is_folio(old_tb) ||
			     swp_tb_to_folio(old_tb) != folio);
		if (__swp_tb_get_count(old_tb))
			folio_swapped = true;
		else
			need_free = true;
		/* If shadow is NULL, we set an empty shadow. */
		__swap_table_set(ci, ci_off, shadow_to_swp_tb(shadow,
				 __swp_tb_get_flags(old_tb)));
	} while (++ci_off < ci_end);

	folio->swap.val = 0;
	folio_clear_swapcache(folio);

	if (!folio_swapped) {
		__swap_cluster_free_entries(si, ci, ci_start, nr_pages);
	} else if (need_free) {
		ci_off = ci_start;
		do {
			if (!__swp_tb_get_count(__swap_table_get(ci, ci_off)))
				__swap_cluster_free_entries(si, ci, ci_off, 1);
		} while (++ci_off < ci_end);
	}
}

/**
 * __swap_cache_del_folio - Removes a folio from the swap cache.
 * @ci: The locked swap cluster.
 * @folio: The folio.
 * @entry: The first swap entry that the folio corresponds to.
 * @shadow: shadow value to be filled in the swap cache.
 *
 * Removes a folio from the swap cache and fills a shadow in place.
 * This won't put the folio's refcount. The caller has to do that.
 *
 * Context: Caller must ensure the folio is locked and in the swap cache
 * using the index of @entry, and lock the cluster that holds the entries.
 */
void __swap_cache_del_folio(struct swap_cluster_info *ci, struct folio *folio,
			    swp_entry_t entry, void *shadow)
{
	unsigned long nr_pages = folio_nr_pages(folio);

	__swap_cache_do_del_folio(ci, folio, entry, shadow);
	node_stat_mod_folio(folio, NR_FILE_PAGES, -nr_pages);
	lruvec_stat_mod_folio(folio, NR_SWAPCACHE, -nr_pages);
}

/**
 * swap_cache_del_folio - Removes a folio from the swap cache.
 * @folio: The folio.
 *
 * Same as __swap_cache_del_folio, but handles lock and refcount. The
 * caller must ensure the folio is either clean or has a swap count
 * equal to zero, or it may cause data loss.
 *
 * Context: Caller must ensure the folio is locked and in the swap cache.
 */
void swap_cache_del_folio(struct folio *folio)
{
	struct swap_cluster_info *ci;
	swp_entry_t entry = folio->swap;

	ci = swap_cluster_lock(__swap_entry_to_info(entry), swp_offset(entry));
	__swap_cache_del_folio(ci, folio, entry, NULL);
	swap_cluster_unlock(ci);

	folio_ref_sub(folio, folio_nr_pages(folio));
}

/**
 * __swap_cache_replace_folio - Replace a folio in the swap cache.
 * @ci: The locked swap cluster.
 * @old: The old folio to be replaced.
 * @new: The new folio.
 *
 * Replace an existing folio in the swap cache with a new folio. The
 * caller is responsible for setting up the new folio's flag and swap
 * entries. Replacement will take the new folio's swap entry value as
 * the starting offset to override all slots covered by the new folio.
 *
 * Context: Caller must ensure both folios are locked, and lock the
 * cluster that holds the old folio to be replaced.
 */
void __swap_cache_replace_folio(struct swap_cluster_info *ci,
				struct folio *old, struct folio *new)
{
	swp_entry_t entry = new->swap;
	unsigned long nr_pages = folio_nr_pages(new);
	unsigned int ci_off = swp_cluster_offset(entry);
	unsigned int ci_end = ci_off + nr_pages;
	unsigned long pfn = folio_pfn(new);
	unsigned long old_tb;

	VM_WARN_ON_ONCE(!folio_test_swapcache(old) || !folio_test_swapcache(new));
	VM_WARN_ON_ONCE(!folio_test_locked(old) || !folio_test_locked(new));
	VM_WARN_ON_ONCE(!entry.val);

	/* Swap cache still stores N entries instead of a high-order entry */
	do {
		old_tb = __swap_table_get(ci, ci_off);
		WARN_ON_ONCE(!swp_tb_is_folio(old_tb) || swp_tb_to_folio(old_tb) != old);
		__swap_table_set(ci, ci_off, pfn_to_swp_tb(pfn, __swp_tb_get_flags(old_tb)));
	} while (++ci_off < ci_end);

	/*
	 * If the old folio is partially replaced (e.g., splitting a large
	 * folio, the old folio is shrunk, and new split sub folios replace
	 * the shrunk part), ensure the new folio doesn't overlap it.
	 */
	if (IS_ENABLED(CONFIG_DEBUG_VM) &&
	    folio_order(old) != folio_order(new)) {
		ci_off = swp_cluster_offset(old->swap);
		ci_end = ci_off + folio_nr_pages(old);
		while (ci_off++ < ci_end)
			WARN_ON_ONCE(swp_tb_to_folio(__swap_table_get(ci, ci_off)) != old);
	}
}

/*
 * Try to allocate a folio of given order in the swap cache.
 *
 * This helper resolves the potential races of swap allocation
 * and prepares a folio to be used for swap IO. May return following
 * value:
 *
 * -ENOMEM / -EBUSY: Order is too large or in conflict with sub slot,
 *                   caller should shrink the order and retry
 * -ENOENT / -EEXIST: Target swap entry is unavailable or cached, the caller
 *                    should abort or try to use the cached folio instead
 */
static struct folio *__swap_cache_alloc(struct swap_cluster_info *ci,
					swp_entry_t targ_entry, gfp_t gfp,
					unsigned int order, struct vm_fault *vmf,
					struct mempolicy *mpol, pgoff_t ilx,
					bool defer_memcg1_swapin)
{
	int err;
	swp_entry_t entry;
	struct folio *folio;
	void *shadow = NULL;
	unsigned short memcg_id;
	unsigned long address, nr_pages = 1UL << order;
	struct vm_area_struct *vma = vmf ? vmf->vma : NULL;

	VM_WARN_ON_ONCE(nr_pages > SWAPFILE_CLUSTER);
	entry.val = round_down(targ_entry.val, nr_pages);

	/* Check if the slot and range are available, skip allocation if not */
	spin_lock(&ci->lock);
	err = __swap_cache_add_check(ci, targ_entry, nr_pages, NULL, NULL);
	spin_unlock(&ci->lock);
	if (unlikely(err))
		return ERR_PTR(err);

	/*
	 * Limit THP gfp. The limitation is a no-op for typical
	 * GFP_HIGHUSER_MOVABLE but matters for shmem.
	 */
	if (order)
		gfp = thp_shmem_limit_gfp_mask(vma_thp_gfp_mask(vma), gfp);

	if (mpol || !vmf) {
		folio = folio_alloc_mpol(gfp, order, mpol, ilx, numa_node_id());
	} else {
		address = round_down(vmf->address, PAGE_SIZE << order);
		folio = vma_alloc_folio(gfp, order, vmf->vma, address);
	}
	if (unlikely(!folio))
		return ERR_PTR(-ENOMEM);

	/* Double check the range is still not in conflict */
	spin_lock(&ci->lock);
	err = __swap_cache_add_check(ci, targ_entry, nr_pages, &shadow, &memcg_id);
	if (unlikely(err)) {
		spin_unlock(&ci->lock);
		folio_put(folio);
		return ERR_PTR(err);
	}

	__folio_set_locked(folio);
	__folio_set_swapbacked(folio);
	__swap_cache_do_add_folio(ci, folio, entry);
	spin_unlock(&ci->lock);

	if (mem_cgroup_swapin_charge_folio(folio, memcg_id,
					   vmf ? vmf->vma->vm_mm : NULL, gfp)) {
		spin_lock(&ci->lock);
		__swap_cache_do_del_folio(ci, folio, entry, shadow);
		spin_unlock(&ci->lock);
		folio_unlock(folio);
		/* nr_pages refs from swap cache, 1 from allocation */
		folio_put_refs(folio, nr_pages + 1);
		count_mthp_stat(order, MTHP_STAT_SWPIN_FALLBACK_CHARGE);
		return ERR_PTR(-ENOMEM);
	}

	/* memsw uncharges swap when folio is added to swap cache */
	if (!defer_memcg1_swapin || !order)
		memcg1_swapin(folio);
	if (shadow)
		workingset_refault(folio, shadow);

	node_stat_mod_folio(folio, NR_FILE_PAGES, nr_pages);
	lruvec_stat_mod_folio(folio, NR_SWAPCACHE, nr_pages);

	/* Caller will initiate read into locked new_folio */
	folio_add_lru(folio);
	return folio;
}

/**
 * swap_cache_alloc_folio - Allocate folio for swapped out slot in swap cache.
 * @targ_entry: swap entry indicating the target slot
 * @gfp: memory allocation flags
 * @orders: allocation orders, must be non zero
 * @vmf: fault information
 * @mpol: NUMA memory allocation policy to be applied
 * @ilx: NUMA interleave index, for use only when MPOL_INTERLEAVE
 *
 * Allocate a folio in the swap cache for one swap slot, typically before
 * doing IO (e.g. swap in or zswap writeback). The swap slot indicated by
 * @targ_entry must have a non-zero swap count (swapped out).
 *
 * Context: Caller must protect the swap device with reference count or locks.
 * Return: Returns the folio if allocation succeeded and folio is in the swap
 * cache. Returns error code if failed due to race, OOM or invalid arguments.
 */
static struct folio *__swap_cache_alloc_folio(swp_entry_t targ_entry,
					      gfp_t gfp, unsigned long orders,
					      struct vm_fault *vmf,
					      struct mempolicy *mpol,
					      pgoff_t ilx,
					      bool defer_memcg1_swapin)
{
	int order, err;
	struct folio *ret;
	struct swap_cluster_info *ci;

	ci = __swap_entry_to_cluster(targ_entry);
	order = highest_order(orders);

	/* orders must be non-zero, and must not exceed cluster size. */
	if (WARN_ON_ONCE(!orders || (1UL << order) > SWAPFILE_CLUSTER))
		return ERR_PTR(-EINVAL);

	do {
		ret = __swap_cache_alloc(ci, targ_entry, gfp, order,
					 vmf, mpol, ilx,
					 defer_memcg1_swapin);
		if (!IS_ERR(ret))
			break;
		err = PTR_ERR(ret);
		if (!order || (err && err != -EBUSY && err != -ENOMEM))
			break;
		count_mthp_stat(order, MTHP_STAT_SWPIN_FALLBACK);
		order = next_order(&orders, order);
	} while (orders);

	return ret;
}

struct folio *swap_cache_alloc_folio(swp_entry_t targ_entry, gfp_t gfp,
				     unsigned long orders, struct vm_fault *vmf,
				     struct mempolicy *mpol, pgoff_t ilx)
{
	return __swap_cache_alloc_folio(targ_entry, gfp, orders, vmf,
					mpol, ilx, false);
}

static struct folio *swap_cache_alloc_speculative_folio(swp_entry_t targ_entry,
							gfp_t gfp,
							unsigned long orders,
							struct vm_fault *vmf,
							struct mempolicy *mpol,
							pgoff_t ilx)
{
	/*
	 * Speculative large swapin may drop this fresh swapcache folio and
	 * retry order-0 after backend or page-table revalidation. Keep the
	 * cgroup v1 memsw swap owner until the caller commits the folio.
	 */
	return __swap_cache_alloc_folio(targ_entry, gfp, orders, vmf,
					mpol, ilx, true);
}

/*
 * Initial conservative cap for speculative zswap large swapin. Locality
 * evidence is supplied by the caller or by generic VMA hints; the common
 * swapin layer keeps backend safety and pressure decisions here.
 */
#define SWAPIN_ZSWAP_MAX_SIZE			SZ_64K
#if PAGE_SIZE < SWAPIN_ZSWAP_MAX_SIZE
#define SWAPIN_ZSWAP_MAX_ORDER			\
	ilog2(SWAPIN_ZSWAP_MAX_SIZE / PAGE_SIZE)
#else
#define SWAPIN_ZSWAP_MAX_ORDER			0
#endif

struct zswap_admit_ctx {
	bool pressure_checked;
	bool reclaim_pressure;
};

static bool swapin_zeromap_same(swp_entry_t entry, unsigned int nr_pages)
{
	unsigned int ci_start = swp_cluster_offset(entry);
	struct swap_cluster_info *ci = __swap_entry_to_cluster(entry);
	bool is_zero;
	unsigned int i;

	if (ci_start + nr_pages > SWAPFILE_CLUSTER) {
		VM_WARN_ON_ONCE(1);
		return false;
	}

	rcu_read_lock();
	if (!rcu_dereference(ci->table)) {
		rcu_read_unlock();
		return true;
	}

	is_zero = __swap_table_test_zero(ci, ci_start);
	for (i = 1; i < nr_pages; i++) {
		if (is_zero != __swap_table_test_zero(ci, ci_start + i)) {
			rcu_read_unlock();
			return false;
		}
	}
	rcu_read_unlock();

	return true;
}

static bool swapin_zswap_locality(struct vm_fault *vmf, unsigned int order,
				  unsigned long locality_orders)
{
	struct vm_area_struct *vma = vmf ? vmf->vma : NULL;

	if (!order || order > MAX_PAGE_ORDER)
		return false;

	if (vma && (vma->vm_flags & VM_RAND_READ))
		return false;

	return locality_orders & BIT(order);
}

static bool swapin_zswap_refaulted(swp_entry_t entry, unsigned int nr_pages)
{
	unsigned int type = swp_type(entry);
	pgoff_t offset = swp_offset(entry);
	unsigned int i;

	for (i = 0; i < nr_pages; i++) {
		bool workingset;
		void *shadow;

		shadow = swap_cache_get_shadow(swp_entry(type, offset + i));
		if (!shadow)
			continue;
		if (workingset_test_recent(shadow, false, &workingset, false) &&
		    workingset)
			return true;
	}

	return false;
}

static bool swapin_zswap_admit(swp_entry_t entry,
			       unsigned int order, unsigned int nr_pages,
			       struct vm_fault *vmf,
			       unsigned long locality_orders,
			       struct zswap_admit_ctx *ctx)
{
	if (order > SWAPIN_ZSWAP_MAX_ORDER)
		return false;

	/*
	 * Treat zswap-backed large swapin as speculative. The common layer
	 * consumes caller-provided locality orders, but does not inspect
	 * anon-specific PTE state or shmem-specific mapping state directly.
	 */
	if (!swapin_zswap_locality(vmf, order, locality_orders))
		return false;

	/*
	 * A recent workingset refault shadow in the target range means reclaim
	 * already saw churn there. Keep the refault path narrow instead of
	 * speculatively decompressing neighbouring slots.
	 */
	if (swapin_zswap_refaulted(entry, nr_pages))
		return false;

	if (!ctx->pressure_checked) {
		ctx->reclaim_pressure = zswap_pool_reclaim_pressure();
		ctx->pressure_checked = true;
	}
	if (ctx->reclaim_pressure)
		return false;

	return true;
}

static unsigned long swapin_admit_orders(swp_entry_t entry,
					 unsigned long orders,
					 struct vm_fault *vmf,
					 unsigned long locality_orders,
					 bool zswap_only)
{
	unsigned long candidates = orders & ~BIT(0);
	unsigned long admitted = zswap_only ? 0 : orders & BIT(0);
	enum zswap_range_state fault_zswap_state = ZSWAP_RANGE_NEVER_ENABLED;
	struct zswap_admit_ctx zswap_ctx = {};
	bool fault_zswap_checked = false;
	int order;

	if (!candidates)
		return zswap_only ? 0 : orders;

	while (candidates) {
		enum zswap_range_state state;
		unsigned int nr_pages;
		swp_entry_t range_entry;
		bool zswap_locality;
		bool admit = false;

		order = fls_long(candidates) - 1;
		if (order > MAX_PAGE_ORDER) {
			candidates &= ~BIT(order);
			continue;
		}

		nr_pages = 1U << order;
		range_entry = swp_entry(swp_type(entry),
					round_down(swp_offset(entry), nr_pages));
		zswap_locality = order <= SWAPIN_ZSWAP_MAX_ORDER &&
				 swapin_zswap_locality(vmf, order,
						       locality_orders);
		/*
		 * If the faulting slot is already in zswap but this order has
		 * no zswap locality evidence, a larger range covering the fault
		 * cannot be admitted: it is either all-zswap or mixed, and both
		 * require zswap locality. Avoid scanning the whole range on
		 * sparse/random zswap refaults. If the faulting slot is not in
		 * zswap, keep the full classification so all-disk large swapin
		 * can follow the existing policy.
		 */
		if (!zswap_locality) {
			if (zswap_only)
				goto next;
			if (!fault_zswap_checked) {
				fault_zswap_state = zswap_probe_range(entry, 1);
				fault_zswap_checked = true;
			}
			if (fault_zswap_state == ZSWAP_RANGE_ALL_ZSWAP)
				goto next;
		}

		if (!swapin_zeromap_same(range_entry, nr_pages))
			goto next;

		state = zswap_probe_range(range_entry, nr_pages);
		switch (state) {
		case ZSWAP_RANGE_ALL_ZSWAP:
			admit = swapin_zswap_admit(range_entry, order,
						   nr_pages, vmf,
						   locality_orders,
						   &zswap_ctx);
			break;
		case ZSWAP_RANGE_MIXED:
			break;
		case ZSWAP_RANGE_NEVER_ENABLED:
		case ZSWAP_RANGE_NO_ZSWAP:
			admit = !zswap_only;
			break;
		}

next:
		if (admit)
			admitted |= BIT(order);
		else
			count_mthp_stat(order, MTHP_STAT_SWPIN_FALLBACK);
		candidates &= ~BIT(order);
	}

	return admitted ? admitted : (zswap_only ? 0 : BIT(0));
}

static bool zswap_folio_all_zswap(struct folio *folio)
{
	return zswap_probe_range(folio->swap, folio_nr_pages(folio)) ==
	       ZSWAP_RANGE_ALL_ZSWAP;
}

static bool zswap_needs_fallback(struct folio *folio, bool zswap_only)
{
	enum zswap_range_state state;

	if (!folio_test_large(folio))
		return false;

	state = zswap_probe_range(folio->swap, folio_nr_pages(folio));
	if (zswap_only)
		return state != ZSWAP_RANGE_ALL_ZSWAP;

	/*
	 * Admission sees only an advisory zswap snapshot. Recheck after the
	 * large swapcache folio is installed; if the range became mixed, drop
	 * the fresh folio before IO and let order-0 handle each slot.
	 */
	return state == ZSWAP_RANGE_MIXED;
}

/*
 * A speculative large swapin may install PTEs for pages that did not fault.
 * Keep those non-faulting PTEs old so a later anon fault can report
 * PTE-young density as caller-provided locality evidence without storing
 * state in the VMA.
 */
bool swapin_fault_only_young(struct folio *folio)
{
	if (!folio_test_large(folio) || !folio_test_swapcache(folio))
		return false;

	return zswap_folio_all_zswap(folio);
}

/*
 * If we are the only user, then try to free up the swap cache.
 *
 * Its ok to check the swapcache flag without the folio lock
 * here because we are going to recheck again inside
 * folio_free_swap() _with_ the lock.
 * 					- Marcelo
 */
void free_swap_cache(struct folio *folio)
{
	if (folio_test_swapcache(folio) && !folio_mapped(folio) &&
	    folio_trylock(folio)) {
		folio_free_swap(folio);
		folio_unlock(folio);
	}
}

/*
 * Freeing a folio and also freeing any swap cache associated with
 * this folio if it is the last user.
 */
void free_folio_and_swap_cache(struct folio *folio)
{
	free_swap_cache(folio);
	if (!is_huge_zero_folio(folio))
		folio_put(folio);
}

/*
 * Passed an array of pages, drop them all from swapcache and then release
 * them.  They are removed from the LRU and freed if this is their last use.
 */
void free_pages_and_swap_cache(struct encoded_page **pages, int nr)
{
	struct folio_batch folios;
	unsigned int refs[FOLIO_BATCH_SIZE];

	folio_batch_init(&folios);
	for (int i = 0; i < nr; i++) {
		struct folio *folio = page_folio(encoded_page_ptr(pages[i]));

		free_swap_cache(folio);
		refs[folios.nr] = 1;
		if (unlikely(encoded_page_flags(pages[i]) &
			     ENCODED_PAGE_BIT_NR_PAGES_NEXT))
			refs[folios.nr] = encoded_nr_pages(pages[++i]);

		if (folio_batch_add(&folios, folio) == 0)
			folios_put_refs(&folios, refs);
	}
	if (folios.nr)
		folios_put_refs(&folios, refs);
}

static inline bool swap_use_vma_readahead(void)
{
	return READ_ONCE(enable_vma_readahead) && !atomic_read(&nr_rotate_swap);
}

/**
 * swap_update_readahead - Update the readahead statistics of VMA or globally.
 * @folio: the swap cache folio that just got hit.
 * @vma: the VMA that should be updated, could be NULL for global update.
 * @addr: the addr that triggered the swapin, ignored if @vma is NULL.
 */
void swap_update_readahead(struct folio *folio, struct vm_area_struct *vma,
			   unsigned long addr)
{
	bool readahead, vma_ra = swap_use_vma_readahead();

	/*
	 * At the moment, we don't support PG_readahead for anon THP
	 * so let's bail out rather than confusing the readahead stat.
	 */
	if (unlikely(folio_test_large(folio)))
		return;

	readahead = folio_test_clear_readahead(folio);
	if (vma && vma_ra) {
		unsigned long ra_val;
		int win, hits;

		ra_val = GET_SWAP_RA_VAL(vma);
		win = SWAP_RA_WIN(ra_val);
		hits = SWAP_RA_HITS(ra_val);
		if (readahead)
			hits = min_t(int, hits + 1, SWAP_RA_HITS_MAX);
		atomic_long_set(&vma->swap_readahead_info,
				SWAP_RA_VAL(addr, win, hits));
	}

	if (readahead) {
		count_vm_event(SWAP_RA_HIT);
		if (!vma || !vma_ra)
			atomic_inc(&swapin_readahead_hits);
	}
}

static struct folio *swap_cache_read_folio(swp_entry_t entry, gfp_t gfp,
					   struct mempolicy *mpol, pgoff_t ilx,
					   struct swap_iocb **plug, bool readahead)
{
	struct folio *folio;
	int ret;

	do {
		folio = swap_cache_get_folio(entry);
		if (folio)
			return folio;
		folio = swap_cache_alloc_folio(entry, gfp, BIT(0), NULL,
					       mpol, ilx);
	} while (PTR_ERR(folio) == -EEXIST);

	if (IS_ERR_OR_NULL(folio))
		return NULL;

	ret = swap_read_folio(folio, plug);
	/*
	 * Swap readahead allocates order-0 folios. -EAGAIN is reserved for
	 * retryable large zswap backend races and should never escape to this
	 * order-0 path.
	 */
	VM_WARN_ON_ONCE(ret == -EAGAIN);
	if (readahead) {
		folio_set_readahead(folio);
		count_vm_event(SWAP_RA);
	}

	return folio;
}

static struct folio *swapin_alloc_read(swp_entry_t entry, gfp_t gfp,
				       unsigned long orders,
				       struct vm_fault *vmf,
				       struct mempolicy *mpol, pgoff_t ilx,
				       bool retry_order0, bool zswap_only)
{
	struct folio *folio;
	int ret;

again:
	do {
		folio = swap_cache_get_folio(entry);
		if (folio)
			return folio;
		folio = swap_cache_alloc_speculative_folio(entry, gfp, orders,
							   vmf, mpol, ilx);
	} while (PTR_ERR(folio) == -EEXIST);

	if (IS_ERR(folio))
		return retry_order0 ? folio : NULL;

	if (zswap_needs_fallback(folio, zswap_only)) {
		count_mthp_stat(folio_order(folio), MTHP_STAT_SWPIN_FALLBACK);
		/*
		 * The folio is newly allocated, locked, clean and not uptodate;
		 * no data has been read into it. Removing it only restores the
		 * swap table entries so the fallback path can resolve a backend
		 * race without attempting speculative large-folio zswapin.
		 */
		swap_cache_del_folio(folio);
		folio_unlock(folio);
		folio_put(folio);
		if (!retry_order0)
			return NULL;
		orders = BIT(0);
		goto again;
	}

	ret = swap_read_folio(folio, NULL);
	if (ret == -EAGAIN) {
		count_mthp_stat(folio_order(folio), MTHP_STAT_SWPIN_FALLBACK);
		swap_cache_del_folio(folio);
		folio_unlock(folio);
		folio_put(folio);
		if (!retry_order0)
			return NULL;
		orders = BIT(0);
		goto again;
	}
	return folio;
}

/**
 * swapin_sync - swap-in one or multiple entries skipping readahead.
 * @entry: swap entry indicating the target slot
 * @gfp: memory allocation flags
 * @orders: allocation orders
 * @locality_orders: orders with caller-provided locality evidence
 * @vmf: fault information
 * @mpol: NUMA memory allocation policy to be applied
 * @ilx: NUMA interleave index, for use only when MPOL_INTERLEAVE
 *
 * This allocates a folio suitable for given @orders, or returns the
 * existing folio in the swap cache for @entry. This initiates the IO, too,
 * if needed. @entry is rounded down if @orders allow large allocation.
 *
 * Context: Caller must ensure @entry is valid and pin the swap device with
 * refcount.
 * Return: Returns the folio on success, error code if failed.
 */
struct folio *swapin_sync(swp_entry_t entry, gfp_t gfp,
			  unsigned long orders,
			  unsigned long locality_orders,
			  struct vm_fault *vmf, struct mempolicy *mpol,
			  pgoff_t ilx)
{
	orders = swapin_admit_orders(entry, orders, vmf,
				     locality_orders, false);
	return swapin_alloc_read(entry, gfp, orders, vmf, mpol, ilx,
				 true, false);
}

static struct folio *swapin_zswap_large(swp_entry_t entry, gfp_t gfp,
					unsigned long orders,
					unsigned long locality_orders,
					struct vm_fault *vmf,
					struct mempolicy *mpol, pgoff_t ilx)
{
	if (READ_ONCE(page_cluster) <= 0)
		return NULL;

	orders = swapin_admit_orders(entry, orders, vmf,
				     locality_orders, true);
	if (!orders)
		return NULL;

	return swapin_alloc_read(entry, gfp, orders, vmf, mpol, ilx,
				 false, true);
}

/*
 * Locate a page of swap in physical memory, reserving swap cache space
 * and reading the disk if it is not already cached.
 * A failure return means that either the page allocation failed or that
 * the swap entry is no longer in use.
 */
struct folio *read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
		struct vm_area_struct *vma, unsigned long addr,
		struct swap_iocb **plug)
{
	struct swap_info_struct *si;
	struct mempolicy *mpol;
	pgoff_t ilx;
	struct folio *folio;

	si = get_swap_device(entry);
	if (!si)
		return NULL;

	mpol = get_vma_policy(vma, addr, 0, &ilx);
	folio = swap_cache_read_folio(entry, gfp_mask, mpol, ilx, plug, false);
	mpol_cond_put(mpol);

	put_swap_device(si);
	return folio;
}

static unsigned int __swapin_nr_pages(unsigned long prev_offset,
				      unsigned long offset,
				      int hits,
				      int max_pages,
				      int prev_win)
{
	unsigned int pages, last_ra;

	/*
	 * This heuristic has been found to work well on both sequential and
	 * random loads, swapping to hard disk or to SSD: please don't ask
	 * what the "+ 2" means, it just happens to work well, that's all.
	 */
	pages = hits + 2;
	if (pages == 2) {
		/*
		 * We can have no readahead hits to judge by: but must not get
		 * stuck here forever, so check for an adjacent offset instead
		 * (and don't even bother to check whether swap type is same).
		 */
		if (offset != prev_offset + 1 && offset != prev_offset - 1)
			pages = 1;
	} else {
		unsigned int roundup = 4;
		while (roundup < pages)
			roundup <<= 1;
		pages = roundup;
	}

	if (pages > max_pages)
		pages = max_pages;

	/* Don't shrink readahead too fast */
	last_ra = prev_win / 2;
	if (pages < last_ra)
		pages = last_ra;

	return pages;
}

static unsigned long swapin_nr_pages(unsigned long offset)
{
	static unsigned long prev_offset;
	unsigned int hits, pages, max_pages;
	static atomic_t last_readahead_pages;

	max_pages = 1 << READ_ONCE(page_cluster);
	if (max_pages <= 1)
		return 1;

	hits = atomic_xchg(&swapin_readahead_hits, 0);
	pages = __swapin_nr_pages(READ_ONCE(prev_offset), offset, hits,
				  max_pages,
				  atomic_read(&last_readahead_pages));
	if (!hits)
		WRITE_ONCE(prev_offset, offset);
	atomic_set(&last_readahead_pages, pages);

	return pages;
}

struct swap_cluster_ra {
	unsigned long start_offset;
	unsigned long end_offset;
	bool readahead;
};

static void swap_cluster_ra_prepare(swp_entry_t entry,
				    struct swap_cluster_ra *ra)
{
	struct swap_info_struct *si = __swap_entry_to_info(entry);
	unsigned long entry_offset = swp_offset(entry);
	unsigned long mask;

	mask = swapin_nr_pages(entry_offset) - 1;
	ra->readahead = !!mask;
	ra->start_offset = entry_offset;
	ra->end_offset = entry_offset;
	if (!mask)
		return;

	/* Read a page_cluster sized and aligned cluster around offset. */
	ra->start_offset = entry_offset & ~mask;
	ra->end_offset = entry_offset | mask;
	if (!ra->start_offset)	/* First page is swap header. */
		ra->start_offset++;
	if (ra->end_offset >= si->max)
		ra->end_offset = si->max - 1;
}

static unsigned long swap_cluster_ra_orders(swp_entry_t entry,
					    unsigned long orders,
					    const struct swap_cluster_ra *ra)
{
	unsigned long admitted = 0;
	unsigned long candidates = orders & ~BIT(0);
	unsigned long entry_offset = swp_offset(entry);
	int order;

	if (!ra->readahead)
		return 0;

	while (candidates) {
		unsigned long nr_pages;
		unsigned long start_offset;
		unsigned long end_offset;

		order = fls_long(candidates) - 1;
		if (order > MAX_PAGE_ORDER) {
			candidates &= ~BIT(order);
			continue;
		}

		nr_pages = 1UL << order;
		start_offset = round_down(entry_offset, nr_pages);
		end_offset = start_offset + nr_pages - 1;
		if (start_offset >= ra->start_offset &&
		    end_offset <= ra->end_offset)
			admitted |= BIT(order);
		candidates &= ~BIT(order);
	}

	return admitted;
}

static bool swapin_readahead_skip(unsigned long index,
				  unsigned long skip_start,
				  unsigned long skip_end)
{
	return skip_start < skip_end &&
	       index >= skip_start && index < skip_end;
}

/**
 * swap_cluster_readahead_win - swap in pages from a prepared swap window
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @mpol: NUMA memory allocation policy to be applied
 * @ilx: NUMA interleave index, for use only when MPOL_INTERLEAVE
 * @ra: readahead window prepared by swap_cluster_ra_prepare()
 * @skip_start: first offset already covered by @target_folio
 * @skip_end: offset after the already covered range
 * @target_folio: target folio to return after queueing the rest of the window
 *
 * Returns the struct folio for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read an aligned block of
 * (1 << page_cluster) entries in the swap area. This method is chosen
 * because it doesn't cost us any seek time.  We also make sure to queue
 * the 'original' request together with the readahead ones...
 *
 * Note: it is intentional that the same NUMA policy and interleave index
 * are used for every page of the readahead: neighbouring pages on swap
 * are fairly likely to have been swapped out from the same node.
 */
static struct folio *swap_cluster_readahead_win(swp_entry_t entry,
						gfp_t gfp_mask,
						struct mempolicy *mpol,
						pgoff_t ilx,
						const struct swap_cluster_ra *ra,
						unsigned long skip_start,
						unsigned long skip_end,
						struct folio *target_folio)
{
	struct folio *folio;
	unsigned long entry_offset = swp_offset(entry);
	unsigned long offset;
	struct blk_plug plug;
	struct swap_iocb *splug = NULL;
	swp_entry_t ra_entry;

	if (!ra->readahead)
		goto skip;

	if (target_folio &&
	    skip_start <= ra->start_offset && skip_end > ra->end_offset)
		goto skip;

	blk_start_plug(&plug);
	for (offset = ra->start_offset; offset <= ra->end_offset; offset++) {
		if (swapin_readahead_skip(offset, skip_start, skip_end)) {
			if (skip_end > ra->end_offset)
				break;
			offset = skip_end - 1;
			continue;
		}

		/* Ok, do the async read-ahead now */
		ra_entry = swp_entry(swp_type(entry), offset);
		folio = swap_cache_read_folio(ra_entry, gfp_mask, mpol, ilx,
					      &splug, offset != entry_offset);
		if (!folio)
			continue;
		folio_put(folio);
	}
	blk_finish_plug(&plug);
	swap_read_unplug(splug);
	lru_add_drain();	/* Push any new pages onto the LRU now */
skip:
	if (target_folio)
		return target_folio;

	/* The page was likely read above, so no need for plugging here */
	return swap_cache_read_folio(entry, gfp_mask, mpol, ilx, NULL, false);
}

struct folio *swap_cluster_readahead(swp_entry_t entry, gfp_t gfp_mask,
				     struct mempolicy *mpol, pgoff_t ilx)
{
	struct swap_cluster_ra ra;

	swap_cluster_ra_prepare(entry, &ra);
	return swap_cluster_readahead_win(entry, gfp_mask, mpol, ilx, &ra,
					 0, 0, NULL);
}

struct swap_vma_ra {
	unsigned long start;
	unsigned long end;
	int win;
};

static int swap_vma_ra_win(struct vm_fault *vmf, unsigned long *start,
			   unsigned long *end)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long ra_val;
	unsigned long faddr, prev_faddr, left, right;
	unsigned int max_win, hits, prev_win, win;

	max_win = 1 << min(READ_ONCE(page_cluster), SWAP_RA_ORDER_CEILING);
	if (max_win == 1)
		return 1;

	faddr = vmf->address;
	ra_val = GET_SWAP_RA_VAL(vma);
	prev_faddr = SWAP_RA_ADDR(ra_val);
	prev_win = SWAP_RA_WIN(ra_val);
	hits = SWAP_RA_HITS(ra_val);
	win = __swapin_nr_pages(PFN_DOWN(prev_faddr), PFN_DOWN(faddr), hits,
				max_win, prev_win);
	atomic_long_set(&vma->swap_readahead_info, SWAP_RA_VAL(faddr, win, 0));
	if (win == 1)
		return 1;

	if (faddr == prev_faddr + PAGE_SIZE)
		left = faddr;
	else if (prev_faddr == faddr + PAGE_SIZE)
		left = faddr - (win << PAGE_SHIFT) + PAGE_SIZE;
	else
		left = faddr - (((win - 1) / 2) << PAGE_SHIFT);
	right = left + (win << PAGE_SHIFT);
	if ((long)left < 0)
		left = 0;
	*start = max3(left, vma->vm_start, faddr & PMD_MASK);
	*end = min3(right, vma->vm_end, (faddr & PMD_MASK) + PMD_SIZE);

	return win;
}

static unsigned long swap_vma_ra_orders(struct vm_fault *vmf,
					unsigned long orders,
					const struct swap_vma_ra *ra)
{
	unsigned long admitted = 0;
	unsigned long candidates = orders & ~BIT(0);
	int order;

	if (ra->win <= 1)
		return 0;

	while (candidates) {
		unsigned long size;
		unsigned long start;
		unsigned long end;

		order = fls_long(candidates) - 1;
		if (order > MAX_PAGE_ORDER) {
			candidates &= ~BIT(order);
			continue;
		}

		size = PAGE_SIZE << order;
		start = ALIGN_DOWN(vmf->address, size);
		end = start + size;
		if (start >= ra->start && end <= ra->end)
			admitted |= BIT(order);
		candidates &= ~BIT(order);
	}

	return admitted;
}

/*
 * Queue swapin for a precomputed VMA readahead window. The window has already
 * been accounted in vma->swap_readahead_info, so fallback after a failed
 * zswap-large attempt does not update readahead state a second time. If
 * @target_folio is already populated, queue only the part of the window outside
 * [@skip_start, @skip_end) and return @target_folio.
 */
static struct folio *swap_vma_readahead_win(swp_entry_t targ_entry,
					    gfp_t gfp_mask,
					    struct mempolicy *mpol,
					    pgoff_t targ_ilx,
					    struct vm_fault *vmf,
					    const struct swap_vma_ra *ra,
					    unsigned long skip_start,
					    unsigned long skip_end,
					    struct folio *target_folio)
{
	struct blk_plug plug;
	struct swap_iocb *splug = NULL;
	struct folio *folio;
	pte_t *pte = NULL, pentry;
	unsigned long start, end, addr;
	pgoff_t ilx = targ_ilx;

	if (ra->win <= 1)
		goto skip;

	start = ra->start;
	end = ra->end;
	if (target_folio && skip_start <= start && skip_end >= end)
		goto skip;

	ilx = targ_ilx - PFN_DOWN(vmf->address - start);

	blk_start_plug(&plug);
	for (addr = start; addr < end; ilx++, addr += PAGE_SIZE) {
		struct swap_info_struct *si = NULL;
		softleaf_t entry;

		if (swapin_readahead_skip(addr, skip_start, skip_end)) {
			unsigned long next = min(skip_end, end);

			if (pte) {
				pte_unmap(pte);
				pte = NULL;
			}
			ilx += PFN_DOWN(next - addr) - 1;
			addr = next - PAGE_SIZE;
			continue;
		}

		if (!pte++) {
			pte = pte_offset_map(vmf->pmd, addr);
			if (!pte)
				break;
		}
		pentry = ptep_get_lockless(pte);
		entry = softleaf_from_pte(pentry);

		if (!softleaf_is_swap(entry))
			continue;
		pte_unmap(pte);
		pte = NULL;
		/*
		 * Readahead entry may come from a device that we are not
		 * holding a reference to, try to grab a reference, or skip.
		 */
		if (swp_type(entry) != swp_type(targ_entry)) {
			si = get_swap_device(entry);
			if (!si)
				continue;
		}
		folio = swap_cache_read_folio(entry, gfp_mask, mpol, ilx,
					      &splug, addr != vmf->address);
		if (si)
			put_swap_device(si);
		if (!folio)
			continue;
		folio_put(folio);
	}
	if (pte)
		pte_unmap(pte);
	blk_finish_plug(&plug);
	swap_read_unplug(splug);
	lru_add_drain();
skip:
	if (target_folio)
		return target_folio;

	/* The folio was likely read above, so no need for plugging here */
	folio = swap_cache_read_folio(targ_entry, gfp_mask, mpol, targ_ilx,
				      NULL, false);
	return folio;
}

/**
 * swapin_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @orders: large folio orders suitable for the faulting entry
 * @vmf: fault information
 *
 * Returns the struct folio for entry and addr, after queueing swapin.
 *
 * This first computes the normal VMA or cluster readahead window. If the
 * window fully covers an aligned all-zswap range containing the fault, that
 * range may be swapped in as one large folio. The remaining window is still
 * queued through the original order-0 readahead path, skipping the already
 * covered target range and without updating readahead state a second time.
 */
struct folio *swapin_readahead(swp_entry_t entry, gfp_t gfp_mask,
				unsigned long orders, struct vm_fault *vmf)
{
	struct mempolicy *mpol;
	pgoff_t ilx;
	struct folio *folio;
	unsigned long ra_orders;
	bool vma_ra;

	mpol = get_vma_policy(vmf->vma, vmf->address, 0, &ilx);
	vma_ra = swap_use_vma_readahead();
	if (vma_ra) {
		struct swap_vma_ra ra = {};
		unsigned long skip_start = 0;
		unsigned long skip_end = 0;

		ra.win = swap_vma_ra_win(vmf, &ra.start, &ra.end);
		ra_orders = swap_vma_ra_orders(vmf, orders, &ra);
		if (ra_orders) {
			folio = swapin_zswap_large(entry, gfp_mask, ra_orders,
						   ra_orders, vmf, mpol, ilx);
			if (folio) {
				skip_start = ALIGN_DOWN(vmf->address,
							folio_size(folio));
				skip_end = skip_start + folio_size(folio);
				folio = swap_vma_readahead_win(entry, gfp_mask,
							       mpol, ilx, vmf,
							       &ra, skip_start,
							       skip_end, folio);
				goto out;
			}
		}
		folio = swap_vma_readahead_win(entry, gfp_mask, mpol, ilx,
					       vmf, &ra, 0, 0, NULL);
	} else {
		struct swap_cluster_ra ra;
		unsigned long skip_start = 0;
		unsigned long skip_end = 0;

		swap_cluster_ra_prepare(entry, &ra);
		ra_orders = swap_cluster_ra_orders(entry, orders, &ra);
		if (ra_orders) {
			folio = swapin_zswap_large(entry, gfp_mask, ra_orders,
						   ra_orders, vmf, mpol, ilx);
			if (folio) {
				skip_start = swp_offset(folio->swap);
				skip_end = skip_start + folio_nr_pages(folio);
				folio = swap_cluster_readahead_win(entry,
								   gfp_mask,
								   mpol, ilx,
								   &ra,
								   skip_start,
								   skip_end,
								   folio);
				goto out;
			}
		}
		folio = swap_cluster_readahead_win(entry, gfp_mask, mpol, ilx,
						   &ra, 0, 0, NULL);
	}
out:
	mpol_cond_put(mpol);

	return folio;
}

#ifdef CONFIG_SYSFS
static ssize_t vma_ra_enabled_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", str_true_false(enable_vma_readahead));
}
static ssize_t vma_ra_enabled_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	ssize_t ret;

	ret = kstrtobool(buf, &enable_vma_readahead);
	if (ret)
		return ret;

	return count;
}
static struct kobj_attribute vma_ra_enabled_attr = __ATTR_RW(vma_ra_enabled);

static struct attribute *swap_attrs[] = {
	&vma_ra_enabled_attr.attr,
	NULL,
};

static const struct attribute_group swap_attr_group = {
	.attrs = swap_attrs,
};

static int __init swap_init(void)
{
	int err;
	struct kobject *swap_kobj;

	swap_kobj = kobject_create_and_add("swap", mm_kobj);
	if (!swap_kobj) {
		pr_err("failed to create swap kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(swap_kobj, &swap_attr_group);
	if (err) {
		pr_err("failed to register swap group\n");
		goto delete_obj;
	}
	/* Swap cache writeback is LRU based, no tags for it */
	mapping_set_no_writeback_tags(&swap_space);
	return 0;

delete_obj:
	kobject_put(swap_kobj);
	return err;
}
subsys_initcall(swap_init);
#endif
