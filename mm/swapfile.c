// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/swapfile.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 */

#include <linux/blkdev.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/hugetlb.h>
#include <linux/mman.h>
#include <linux/slab.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/vmalloc.h>
#include <linux/pagemap.h>
#include <linux/namei.h>
#include <linux/shmem_fs.h>
#include <linux/blk-cgroup.h>
#include <linux/random.h>
#include <linux/writeback.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/init.h>
#include <linux/ksm.h>
#include <linux/rmap.h>
#include <linux/security.h>
#include <linux/backing-dev.h>
#include <linux/mutex.h>
#include <linux/capability.h>
#include <linux/syscalls.h>
#include <linux/memcontrol.h>
#include <linux/poll.h>
#include <linux/oom.h>
#include <linux/swapfile.h>
#include <linux/export.h>
#include <linux/sort.h>
#include <linux/completion.h>
#include <linux/suspend.h>
#include <linux/zswap.h>
#include <linux/plist.h>

#include <asm/tlbflush.h>
#include <linux/leafops.h>
#include <linux/swap_cgroup.h>
#include "swap_table.h"
#include "internal.h"
#include "swap.h"

static void swap_slots_free(struct swap_info_struct *si,
			      struct swap_cluster_info *ci,
			      swp_slot_t slot, unsigned int nr_pages);
static void swap_range_alloc(struct swap_info_struct *si,
			     unsigned int nr_entries);
static bool folio_swapcache_freeable(struct folio *folio);
static void move_cluster(struct swap_info_struct *si,
			 struct swap_cluster_info *ci, struct list_head *list,
			 enum swap_cluster_flags new_flags);

static DEFINE_SPINLOCK(swap_lock);
static unsigned int nr_swapfiles;
atomic_long_t nr_swap_pages;
/*
 * Some modules use swappable objects and may try to swap them out under
 * memory pressure (via the shrinker). Before doing so, they may wish to
 * check to see if any swap space is available.
 */
EXPORT_SYMBOL_GPL(nr_swap_pages);
/* protected with swap_lock. reading in vm_swap_full() doesn't need lock */
long total_swap_pages;
#define DEF_SWAP_PRIO  -1
unsigned long swapfile_maximum_size;
#ifdef CONFIG_MIGRATION
bool swap_migration_ad_supported;
#endif	/* CONFIG_MIGRATION */

static const char Bad_file[] = "Bad swap file entry ";
static const char Unused_file[] = "Unused swap file entry ";
static const char Bad_offset[] = "Bad swap offset entry ";
static const char Unused_offset[] = "Unused swap offset entry ";

/*
 * all active swap_info_structs
 * protected with swap_lock, and ordered by priority.
 */
static PLIST_HEAD(swap_active_head);

/*
 * all available (active, not full) swap_info_structs
 * protected with swap_avail_lock, ordered by priority.
 * This is used by folio_alloc_swap() instead of swap_active_head
 * because swap_active_head includes all swap_info_structs,
 * but folio_alloc_swap() doesn't need to look at full ones.
 * This uses its own lock instead of swap_lock because when a
 * swap_info_struct changes between not-full/full, it needs to
 * add/remove itself to/from this list, but the swap_info_struct->lock
 * is held and the locking order requires swap_lock to be taken
 * before any swap_info_struct->lock.
 */
static PLIST_HEAD(swap_avail_head);
static DEFINE_SPINLOCK(swap_avail_lock);

struct swap_info_struct *swap_info[MAX_SWAPFILES];

static struct kmem_cache *swap_table_cachep;

static DEFINE_MUTEX(swapon_mutex);

static DECLARE_WAIT_QUEUE_HEAD(proc_poll_wait);
/* Activity counter to indicate that a swapon or swapoff has occurred */
static atomic_t proc_poll_event = ATOMIC_INIT(0);

atomic_t nr_rotate_swap = ATOMIC_INIT(0);

struct percpu_swap_cluster {
	struct swap_info_struct *si[SWAP_NR_ORDERS];
	unsigned long offset[SWAP_NR_ORDERS];
	local_lock_t lock;
};

static DEFINE_PER_CPU(struct percpu_swap_cluster, percpu_swap_cluster) = {
	.si = { NULL },
	.offset = { SWAP_SLOT_INVALID },
	.lock = INIT_LOCAL_LOCK(),
};

/* May return NULL on invalid type, caller must check for NULL return */
static struct swap_info_struct *swap_type_to_info(int type)
{
	if (type >= MAX_SWAPFILES)
		return NULL;
	return READ_ONCE(swap_info[type]); /* rcu_dereference() */
}

/* May return NULL on invalid entry, caller must check for NULL return */
static struct swap_info_struct *swap_slot_to_info(swp_slot_t slot)
{
	return swap_type_to_info(swp_slot_type(slot));
}

static inline unsigned char swap_count(unsigned char ent)
{
	return ent;
}

/*
 * Use the second highest bit of inuse_pages counter as the indicator
 * if one swap device is on the available plist, so the atomic can
 * still be updated arithmetically while having special data embedded.
 *
 * inuse_pages counter is the only thing indicating if a device should
 * be on avail_lists or not (except swapon / swapoff). By embedding the
 * off-list bit in the atomic counter, updates no longer need any lock
 * to check the list status.
 *
 * This bit will be set if the device is not on the plist and not
 * usable, will be cleared if the device is on the plist.
 */
#define SWAP_USAGE_OFFLIST_BIT (1UL << (BITS_PER_TYPE(atomic_t) - 2))
#define SWAP_USAGE_COUNTER_MASK (~SWAP_USAGE_OFFLIST_BIT)
static long swap_usage_in_pages(struct swap_info_struct *si)
{
	return atomic_long_read(&si->inuse_pages) & SWAP_USAGE_COUNTER_MASK;
}

/* Reclaim the swap entry anyway if possible */
#define TTRS_ANYWAY		0x1
/*
 * Reclaim the swap entry if there are no more mappings of the
 * corresponding page
 */
#define TTRS_UNMAPPED		0x2
/* Reclaim the swap entry if swap is getting full */
#define TTRS_FULL		0x4

static bool swap_only_has_cache(struct swap_info_struct *si,
			      unsigned long offset, int nr_pages)
{
	swp_entry_t entry = swp_slot_to_swp_entry(swp_slot(si->type, offset));

	return vswap_only_has_cache(entry, nr_pages);
}

static bool swap_cache_only(struct swap_info_struct *si, unsigned long offset)
{
	return swap_only_has_cache(si, offset, 1);
}

/*
 * returns number of pages in the folio that backs the swap entry. If positive,
 * the folio was reclaimed. If negative, the folio was not reclaimed. If 0, no
 * folio was associated with the swap entry.
 */
static int __try_to_reclaim_swap(struct swap_info_struct *si,
				 unsigned long offset, unsigned long flags)
{
	const swp_slot_t slot = swp_slot(si->type, offset);
	swp_entry_t entry;
	struct swap_cluster_info *ci;
	struct folio *folio;
	int ret, nr_pages;
	bool need_reclaim;

again:
	entry = swp_slot_to_swp_entry(slot);
	folio = swap_cache_get_folio(entry);
	if (!folio)
		return 0;

	nr_pages = folio_nr_pages(folio);
	ret = -nr_pages;

	/*
	 * We hold a folio lock here. We have to use trylock for
	 * avoiding deadlock. This is a special case and you should
	 * use folio_free_swap() with explicit folio_lock() in usual
	 * operations.
	 */
	if (!folio_trylock(folio))
		goto out;

	/*
	 * Offset could point to the middle of a large folio, or folio
	 * may no longer point to the expected offset before it's locked.
	 * Additionally, the virtual swap entry may no longer be backed
	 * by the same physical swap slot.
	 */
	if (!folio_matches_swap_slot(folio, entry, slot)) {
		folio_unlock(folio);
		folio_put(folio);
		goto again;
	}
	offset = swp_slot_offset(swp_entry_to_swp_slot(folio->swap));

	need_reclaim = ((flags & TTRS_ANYWAY) ||
			((flags & TTRS_UNMAPPED) && !folio_mapped(folio)) ||
			((flags & TTRS_FULL) && mem_cgroup_swap_full(folio)));
	if (!need_reclaim || !folio_swapcache_freeable(folio))
		goto out_unlock;

	/*
	 * It's safe to delete the folio from swap cache only if the folio's
	 * swap slots have no page table reference or pending writeback.
	 */
	ci = swap_cluster_lock(si, offset);
	need_reclaim = swap_only_has_cache(si, offset, nr_pages);
	swap_cluster_unlock(ci);
	if (!need_reclaim)
		goto out_unlock;

	swap_cache_del_folio(folio);
	folio_set_dirty(folio);
	ret = nr_pages;
out_unlock:
	folio_unlock(folio);
out:
	folio_put(folio);
	return ret;
}

static inline struct swap_extent *first_se(struct swap_info_struct *sis)
{
	struct rb_node *rb = rb_first(&sis->swap_extent_root);
	return rb_entry(rb, struct swap_extent, rb_node);
}

static inline struct swap_extent *next_se(struct swap_extent *se)
{
	struct rb_node *rb = rb_next(&se->rb_node);
	return rb ? rb_entry(rb, struct swap_extent, rb_node) : NULL;
}

/*
 * swapon tell device that all the old swap contents can be discarded,
 * to allow the swap device to optimize its wear-levelling.
 */
static int discard_swap(struct swap_info_struct *si)
{
	struct swap_extent *se;
	sector_t start_block;
	sector_t nr_blocks;
	int err = 0;

	/* Do not discard the swap header page! */
	se = first_se(si);
	start_block = (se->start_block + 1) << (PAGE_SHIFT - 9);
	nr_blocks = ((sector_t)se->nr_pages - 1) << (PAGE_SHIFT - 9);
	if (nr_blocks) {
		err = blkdev_issue_discard(si->bdev, start_block,
				nr_blocks, GFP_KERNEL);
		if (err)
			return err;
		cond_resched();
	}

	for (se = next_se(se); se; se = next_se(se)) {
		start_block = se->start_block << (PAGE_SHIFT - 9);
		nr_blocks = (sector_t)se->nr_pages << (PAGE_SHIFT - 9);

		err = blkdev_issue_discard(si->bdev, start_block,
				nr_blocks, GFP_KERNEL);
		if (err)
			break;

		cond_resched();
	}
	return err;		/* That will often be -EOPNOTSUPP */
}

static struct swap_extent *
offset_to_swap_extent(struct swap_info_struct *sis, unsigned long offset)
{
	struct swap_extent *se;
	struct rb_node *rb;

	rb = sis->swap_extent_root.rb_node;
	while (rb) {
		se = rb_entry(rb, struct swap_extent, rb_node);
		if (offset < se->start_page)
			rb = rb->rb_left;
		else if (offset >= se->start_page + se->nr_pages)
			rb = rb->rb_right;
		else
			return se;
	}
	/* It *must* be present */
	BUG();
}

sector_t swap_folio_sector(struct folio *folio)
{
	swp_slot_t slot = swp_entry_to_swp_slot(folio->swap);
	struct swap_info_struct *sis = __swap_slot_to_info(slot);
	struct swap_extent *se;
	sector_t sector;
	pgoff_t offset = swp_slot_offset(slot);

	se = offset_to_swap_extent(sis, offset);
	sector = se->start_block + (offset - se->start_page);
	return sector << (PAGE_SHIFT - 9);
}

/*
 * swap allocation tell device that a cluster of swap can now be discarded,
 * to allow the swap device to optimize its wear-levelling.
 */
static void discard_swap_cluster(struct swap_info_struct *si,
				 pgoff_t start_page, pgoff_t nr_pages)
{
	struct swap_extent *se = offset_to_swap_extent(si, start_page);

	while (nr_pages) {
		pgoff_t offset = start_page - se->start_page;
		sector_t start_block = se->start_block + offset;
		sector_t nr_blocks = se->nr_pages - offset;

		if (nr_blocks > nr_pages)
			nr_blocks = nr_pages;
		start_page += nr_blocks;
		nr_pages -= nr_blocks;

		start_block <<= PAGE_SHIFT - 9;
		nr_blocks <<= PAGE_SHIFT - 9;
		if (blkdev_issue_discard(si->bdev, start_block,
					nr_blocks, GFP_NOIO))
			break;

		se = next_se(se);
	}
}

#define LATENCY_LIMIT		256

static inline bool cluster_is_empty(struct swap_cluster_info *info)
{
	return info->count == 0;
}

static inline bool cluster_is_discard(struct swap_cluster_info *info)
{
	return info->flags == CLUSTER_FLAG_DISCARD;
}

static inline bool cluster_table_is_alloced(struct swap_cluster_info *ci)
{
	return rcu_dereference_protected(ci->table, lockdep_is_held(&ci->lock));
}

static inline bool cluster_is_usable(struct swap_cluster_info *ci, int order)
{
	if (unlikely(ci->flags > CLUSTER_FLAG_USABLE))
		return false;
	if (!cluster_table_is_alloced(ci))
		return false;
	if (!order)
		return true;
	return cluster_is_empty(ci) || order == ci->order;
}

static inline unsigned int cluster_index(struct swap_info_struct *si,
					 struct swap_cluster_info *ci)
{
	return ci - si->cluster_info;
}

static inline unsigned int cluster_offset(struct swap_info_struct *si,
					  struct swap_cluster_info *ci)
{
	return cluster_index(si, ci) * SWAPFILE_CLUSTER;
}

static struct swap_table *swap_table_alloc(gfp_t gfp)
{
	struct folio *folio;

	if (!SWP_TABLE_USE_PAGE)
		return kmem_cache_zalloc(swap_table_cachep, gfp);

	folio = folio_alloc(gfp | __GFP_ZERO, 0);
	if (folio)
		return folio_address(folio);
	return NULL;
}

static void swap_table_free_folio_rcu_cb(struct rcu_head *head)
{
	struct folio *folio;

	folio = page_folio(container_of(head, struct page, rcu_head));
	folio_put(folio);
}

static void swap_table_free(struct swap_table *table)
{
	if (!SWP_TABLE_USE_PAGE) {
		kmem_cache_free(swap_table_cachep, table);
		return;
	}

	call_rcu(&(folio_page(virt_to_folio(table), 0)->rcu_head),
		 swap_table_free_folio_rcu_cb);
}

static void swap_cluster_free_table(struct swap_cluster_info *ci)
{
	unsigned int ci_off;
	struct swap_table *table;

	/* Only empty cluster's table is allow to be freed  */
	lockdep_assert_held(&ci->lock);
	VM_WARN_ON_ONCE(!cluster_is_empty(ci));
	for (ci_off = 0; ci_off < SWAPFILE_CLUSTER; ci_off++)
		VM_WARN_ON_ONCE(__swap_table_get(ci, ci_off));
	table = (void *)rcu_dereference_protected(ci->table, true);
	rcu_assign_pointer(ci->table, NULL);

	swap_table_free(table);
}

/*
 * Allocate swap table for one cluster. Attempt an atomic allocation first,
 * then fallback to sleeping allocation.
 */
static struct swap_cluster_info *
swap_cluster_alloc_table(struct swap_info_struct *si,
			 struct swap_cluster_info *ci)
{
	struct swap_table *table;

	/*
	 * Only cluster isolation from the allocator does table allocation.
	 * Swap allocator uses percpu clusters and holds the local lock.
	 */
	lockdep_assert_held(&ci->lock);
	lockdep_assert_held(&this_cpu_ptr(&percpu_swap_cluster)->lock);

	/* The cluster must be free and was just isolated from the free list. */
	VM_WARN_ON_ONCE(ci->flags || !cluster_is_empty(ci));

	table = swap_table_alloc(__GFP_HIGH | __GFP_NOMEMALLOC | __GFP_NOWARN);
	if (table) {
		rcu_assign_pointer(ci->table, table);
		return ci;
	}

	/*
	 * Try a sleep allocation. Each isolated free cluster may cause
	 * a sleep allocation, but there is a limited number of them, so
	 * the potential recursive allocation is limited.
	 */
	spin_unlock(&ci->lock);
	if (!(si->flags & SWP_SOLIDSTATE))
		spin_unlock(&si->global_cluster_lock);
	local_unlock(&percpu_swap_cluster.lock);

	table = swap_table_alloc(__GFP_HIGH | __GFP_NOMEMALLOC | GFP_KERNEL);

	/*
	 * Back to atomic context. We might have migrated to a new CPU with a
	 * usable percpu cluster. But just keep using the isolated cluster to
	 * make things easier. Migration indicates a slight change of workload
	 * so using a new free cluster might not be a bad idea, and the worst
	 * could happen with ignoring the percpu cluster is fragmentation,
	 * which is acceptable since this fallback and race is rare.
	 */
	local_lock(&percpu_swap_cluster.lock);
	if (!(si->flags & SWP_SOLIDSTATE))
		spin_lock(&si->global_cluster_lock);
	spin_lock(&ci->lock);

	/* Nothing except this helper should touch a dangling empty cluster. */
	if (WARN_ON_ONCE(cluster_table_is_alloced(ci))) {
		if (table)
			swap_table_free(table);
		return ci;
	}

	if (!table) {
		move_cluster(si, ci, &si->free_clusters, CLUSTER_FLAG_FREE);
		spin_unlock(&ci->lock);
		return NULL;
	}

	rcu_assign_pointer(ci->table, table);
	return ci;
}

static void move_cluster(struct swap_info_struct *si,
			 struct swap_cluster_info *ci, struct list_head *list,
			 enum swap_cluster_flags new_flags)
{
	VM_WARN_ON(ci->flags == new_flags);

	BUILD_BUG_ON(1 << sizeof(ci->flags) * BITS_PER_BYTE < CLUSTER_FLAG_MAX);
	lockdep_assert_held(&ci->lock);

	spin_lock(&si->lock);
	if (ci->flags == CLUSTER_FLAG_NONE)
		list_add_tail(&ci->list, list);
	else
		list_move_tail(&ci->list, list);
	spin_unlock(&si->lock);
	ci->flags = new_flags;
}

/* Add a cluster to discard list and schedule it to do discard */
static void swap_cluster_schedule_discard(struct swap_info_struct *si,
		struct swap_cluster_info *ci)
{
	VM_BUG_ON(ci->flags == CLUSTER_FLAG_FREE);
	move_cluster(si, ci, &si->discard_clusters, CLUSTER_FLAG_DISCARD);
	schedule_work(&si->discard_work);
}

static void __free_cluster(struct swap_info_struct *si, struct swap_cluster_info *ci)
{
	swap_cluster_free_table(ci);
	move_cluster(si, ci, &si->free_clusters, CLUSTER_FLAG_FREE);
	ci->order = 0;
}

/*
 * Isolate and lock the first cluster that is not contented on a list,
 * clean its flag before taken off-list. Cluster flag must be in sync
 * with list status, so cluster updaters can always know the cluster
 * list status without touching si lock.
 *
 * Note it's possible that all clusters on a list are contented so
 * this returns NULL for an non-empty list.
 */
static struct swap_cluster_info *isolate_lock_cluster(
		struct swap_info_struct *si, struct list_head *list)
{
	struct swap_cluster_info *ci, *found = NULL;

	spin_lock(&si->lock);
	list_for_each_entry(ci, list, list) {
		if (!spin_trylock(&ci->lock))
			continue;

		/* We may only isolate and clear flags of following lists */
		VM_BUG_ON(!ci->flags);
		VM_BUG_ON(ci->flags > CLUSTER_FLAG_USABLE &&
			  ci->flags != CLUSTER_FLAG_FULL);

		list_del(&ci->list);
		ci->flags = CLUSTER_FLAG_NONE;
		found = ci;
		break;
	}
	spin_unlock(&si->lock);

	if (found && !cluster_table_is_alloced(found)) {
		/* Only an empty free cluster's swap table can be freed. */
		VM_WARN_ON_ONCE(list != &si->free_clusters);
		VM_WARN_ON_ONCE(!cluster_is_empty(found));
		return swap_cluster_alloc_table(si, found);
	}

	return found;
}

/*
 * Doing discard actually. After a cluster discard is finished, the cluster
 * will be added to free cluster list. Discard cluster is a bit special as
 * they don't participate in allocation or reclaim, so clusters marked as
 * CLUSTER_FLAG_DISCARD must remain off-list or on discard list.
 */
static bool swap_do_scheduled_discard(struct swap_info_struct *si)
{
	struct swap_cluster_info *ci;
	bool ret = false;
	unsigned int idx;

	spin_lock(&si->lock);
	while (!list_empty(&si->discard_clusters)) {
		ci = list_first_entry(&si->discard_clusters, struct swap_cluster_info, list);
		/*
		 * Delete the cluster from list to prepare for discard, but keep
		 * the CLUSTER_FLAG_DISCARD flag, percpu_swap_cluster could be
		 * pointing to it, or ran into by relocate_cluster.
		 */
		list_del(&ci->list);
		idx = cluster_index(si, ci);
		spin_unlock(&si->lock);
		discard_swap_cluster(si, idx * SWAPFILE_CLUSTER,
				SWAPFILE_CLUSTER);

		spin_lock(&ci->lock);
		/*
		 * Discard is done, clear its flags as it's off-list, then
		 * return the cluster to allocation list.
		 */
		ci->flags = CLUSTER_FLAG_NONE;
		__free_cluster(si, ci);
		spin_unlock(&ci->lock);
		ret = true;
		spin_lock(&si->lock);
	}
	spin_unlock(&si->lock);
	return ret;
}

static void swap_discard_work(struct work_struct *work)
{
	struct swap_info_struct *si;

	si = container_of(work, struct swap_info_struct, discard_work);

	swap_do_scheduled_discard(si);
}

static void swap_users_ref_free(struct percpu_ref *ref)
{
	struct swap_info_struct *si;

	si = container_of(ref, struct swap_info_struct, users);
	complete(&si->comp);
}

/*
 * Must be called after freeing if ci->count == 0, moves the cluster to free
 * or discard list.
 */
static void free_cluster(struct swap_info_struct *si, struct swap_cluster_info *ci)
{
	VM_BUG_ON(ci->count != 0);
	VM_BUG_ON(ci->flags == CLUSTER_FLAG_FREE);
	lockdep_assert_held(&ci->lock);

	/*
	 * If the swap is discardable, prepare discard the cluster
	 * instead of free it immediately. The cluster will be freed
	 * after discard.
	 */
	if ((si->flags & (SWP_WRITEOK | SWP_PAGE_DISCARD)) ==
	    (SWP_WRITEOK | SWP_PAGE_DISCARD)) {
		swap_cluster_schedule_discard(si, ci);
		return;
	}

	__free_cluster(si, ci);
}

/*
 * Must be called after freeing if ci->count != 0, moves the cluster to
 * nonfull list.
 */
static void partial_free_cluster(struct swap_info_struct *si,
				 struct swap_cluster_info *ci)
{
	VM_BUG_ON(!ci->count || ci->count == SWAPFILE_CLUSTER);
	lockdep_assert_held(&ci->lock);

	if (ci->flags != CLUSTER_FLAG_NONFULL)
		move_cluster(si, ci, &si->nonfull_clusters[ci->order],
			     CLUSTER_FLAG_NONFULL);
}

/*
 * Must be called after allocation, moves the cluster to full or frag list.
 * Note: allocation doesn't acquire si lock, and may drop the ci lock for
 * reclaim, so the cluster could be any where when called.
 */
static void relocate_cluster(struct swap_info_struct *si,
			     struct swap_cluster_info *ci)
{
	lockdep_assert_held(&ci->lock);

	/* Discard cluster must remain off-list or on discard list */
	if (cluster_is_discard(ci))
		return;

	if (!ci->count) {
		if (ci->flags != CLUSTER_FLAG_FREE)
			free_cluster(si, ci);
	} else if (ci->count != SWAPFILE_CLUSTER) {
		if (ci->flags != CLUSTER_FLAG_FRAG)
			move_cluster(si, ci, &si->frag_clusters[ci->order],
				     CLUSTER_FLAG_FRAG);
	} else {
		if (ci->flags != CLUSTER_FLAG_FULL)
			move_cluster(si, ci, &si->full_clusters,
				     CLUSTER_FLAG_FULL);
	}
}

/*
 * The cluster corresponding to @offset will be accounted as having one bad
 * slot. The cluster will not be added to the free cluster list, and its
 * usage counter will be increased by 1. Only used for initialization.
 */
static int swap_cluster_setup_bad_slot(struct swap_cluster_info *cluster_info,
				       unsigned long offset)
{
	unsigned long idx = offset / SWAPFILE_CLUSTER;
	struct swap_table *table;
	struct swap_cluster_info *ci;

	ci = cluster_info + idx;
	if (!ci->table) {
		table = swap_table_alloc(GFP_KERNEL);
		if (!table)
			return -ENOMEM;
		rcu_assign_pointer(ci->table, table);
	}

	ci->count++;

	WARN_ON(ci->count > SWAPFILE_CLUSTER);
	WARN_ON(ci->flags);

	return 0;
}

static bool cluster_reclaim_range(struct swap_info_struct *si,
				  struct swap_cluster_info *ci,
				  unsigned long start, unsigned long end)
{
	unsigned long offset = start;
	int nr_reclaim;

	spin_unlock(&ci->lock);
	do {
		if (!test_bit(offset, si->swap_map)) {
			offset++;
		} else {
			nr_reclaim = __try_to_reclaim_swap(si, offset, TTRS_ANYWAY);
			if (nr_reclaim > 0)
				offset += nr_reclaim;
			else
				goto out;
		}
	} while (offset < end);
out:
	spin_lock(&ci->lock);
	/*
	 * Recheck the range no matter reclaim succeeded or not, the slot
	 * could have been be freed while we are not holding the lock.
	 */
	return find_next_bit(si->swap_map, end, start) >= end;
}

static bool cluster_scan_range(struct swap_info_struct *si,
			       struct swap_cluster_info *ci,
			       unsigned long start, unsigned int nr_pages,
			       bool *need_reclaim)
{
	unsigned long offset, end = start + nr_pages;

	if (cluster_is_empty(ci))
		return true;

	for (offset = start; offset < end; offset++) {
		/* Bad slots cannot be used for allocation */
		if (test_bit(offset, si->bad_map))
			return false;

		if (!test_bit(offset, si->swap_map))
			continue;

		if (swap_cache_only(si, offset)) {
			if (!vm_swap_full())
				return false;
			*need_reclaim = true;
			continue;
		}
		return false;
	}

	return true;
}

static bool cluster_alloc_range(struct swap_info_struct *si, struct swap_cluster_info *ci,
				unsigned int start, unsigned char usage,
				unsigned int order)
{
	unsigned int nr_pages = 1 << order;

	lockdep_assert_held(&ci->lock);

	if (!(si->flags & SWP_WRITEOK))
		return false;

	/*
	 * The first allocation in a cluster makes the
	 * cluster exclusive to this order
	 */
	if (cluster_is_empty(ci))
		ci->order = order;

	bitmap_set(si->swap_map, start, nr_pages);
	swap_range_alloc(si, nr_pages);
	ci->count += nr_pages;

	return true;
}

/* Try use a new cluster for current CPU and allocate from it. */
static unsigned int alloc_swap_scan_cluster(struct swap_info_struct *si,
					    struct swap_cluster_info *ci,
					    unsigned long offset,
					    unsigned int order,
					    unsigned char usage)
{
	unsigned int next = SWAP_SLOT_INVALID, found = SWAP_SLOT_INVALID;
	unsigned long start = ALIGN_DOWN(offset, SWAPFILE_CLUSTER);
	unsigned long end = min(start + SWAPFILE_CLUSTER, si->max);
	unsigned int nr_pages = 1 << order;
	bool need_reclaim, ret;

	lockdep_assert_held(&ci->lock);

	if (end < nr_pages || ci->count + nr_pages > SWAPFILE_CLUSTER)
		goto out;

	for (end -= nr_pages; offset <= end; offset += nr_pages) {
		need_reclaim = false;
		if (!cluster_scan_range(si, ci, offset, nr_pages, &need_reclaim))
			continue;
		if (need_reclaim) {
			ret = cluster_reclaim_range(si, ci, offset, offset + nr_pages);
			/*
			 * Reclaim drops ci->lock and cluster could be used
			 * by another order. Not checking flag as off-list
			 * cluster has no flag set, and change of list
			 * won't cause fragmentation.
			 */
			if (!cluster_is_usable(ci, order))
				goto out;
			if (cluster_is_empty(ci))
				offset = start;
			/* Reclaim failed but cluster is usable, try next */
			if (!ret)
				continue;
		}
		if (!cluster_alloc_range(si, ci, offset, usage, order))
			break;
		found = offset;
		offset += nr_pages;
		if (ci->count < SWAPFILE_CLUSTER && offset <= end)
			next = offset;
		break;
	}
out:
	relocate_cluster(si, ci);
	swap_cluster_unlock(ci);
	if (si->flags & SWP_SOLIDSTATE) {
		this_cpu_write(percpu_swap_cluster.offset[order], next);
		this_cpu_write(percpu_swap_cluster.si[order], si);
	} else {
		si->global_cluster->next[order] = next;
	}
	return found;
}

static unsigned int alloc_swap_scan_list(struct swap_info_struct *si,
					 struct list_head *list,
					 unsigned int order,
					 unsigned char usage,
					 bool scan_all)
{
	unsigned int found = SWAP_SLOT_INVALID;

	do {
		struct swap_cluster_info *ci = isolate_lock_cluster(si, list);
		unsigned long offset;

		if (!ci)
			break;
		offset = cluster_offset(si, ci);
		found = alloc_swap_scan_cluster(si, ci, offset, order, usage);
		if (found)
			break;
	} while (scan_all);

	return found;
}

static void swap_reclaim_full_clusters(struct swap_info_struct *si, bool force)
{
	long to_scan = 1;
	unsigned long offset, end;
	struct swap_cluster_info *ci;
	int nr_reclaim;

	if (force)
		to_scan = swap_usage_in_pages(si) / SWAPFILE_CLUSTER;

	while ((ci = isolate_lock_cluster(si, &si->full_clusters))) {
		offset = cluster_offset(si, ci);
		end = min(si->max, offset + SWAPFILE_CLUSTER);
		to_scan--;

		while (offset < end) {
			if (swap_cache_only(si, offset)) {
				spin_unlock(&ci->lock);
				nr_reclaim = __try_to_reclaim_swap(si, offset,
								   TTRS_ANYWAY);
				spin_lock(&ci->lock);
				if (nr_reclaim) {
					offset += abs(nr_reclaim);
					continue;
				}
			}
			offset++;
		}

		/* in case no swap cache is reclaimed */
		if (ci->flags == CLUSTER_FLAG_NONE)
			relocate_cluster(si, ci);

		swap_cluster_unlock(ci);
		if (to_scan <= 0)
			break;
	}
}

static void swap_reclaim_work(struct work_struct *work)
{
	struct swap_info_struct *si;

	si = container_of(work, struct swap_info_struct, reclaim_work);

	swap_reclaim_full_clusters(si, true);
}

/*
 * Try to allocate swap entries with specified order and try set a new
 * cluster for current CPU too.
 */
static unsigned long cluster_alloc_swap_slot(struct swap_info_struct *si, int order,
					      unsigned char usage)
{
	struct swap_cluster_info *ci;
	unsigned int offset = SWAP_SLOT_INVALID, found = SWAP_SLOT_INVALID;

	/*
	 * Swapfile is not block device so unable
	 * to allocate large entries.
	 */
	if (order && !(si->flags & SWP_BLKDEV))
		return 0;

	if (!(si->flags & SWP_SOLIDSTATE)) {
		/* Serialize HDD SWAP allocation for each device. */
		spin_lock(&si->global_cluster_lock);
		offset = si->global_cluster->next[order];
		if (offset == SWAP_SLOT_INVALID)
			goto new_cluster;

		ci = swap_cluster_lock(si, offset);
		/* Cluster could have been used by another order */
		if (cluster_is_usable(ci, order)) {
			if (cluster_is_empty(ci))
				offset = cluster_offset(si, ci);
			found = alloc_swap_scan_cluster(si, ci, offset,
							order, usage);
		} else {
			swap_cluster_unlock(ci);
		}
		if (found)
			goto done;
	}

new_cluster:
	/*
	 * If the device need discard, prefer new cluster over nonfull
	 * to spread out the writes.
	 */
	if (si->flags & SWP_PAGE_DISCARD) {
		found = alloc_swap_scan_list(si, &si->free_clusters, order, usage,
					     false);
		if (found)
			goto done;
	}

	if (order < PMD_ORDER) {
		found = alloc_swap_scan_list(si, &si->nonfull_clusters[order],
					     order, usage, true);
		if (found)
			goto done;
	}

	if (!(si->flags & SWP_PAGE_DISCARD)) {
		found = alloc_swap_scan_list(si, &si->free_clusters, order, usage,
					     false);
		if (found)
			goto done;
	}

	/* Try reclaim full clusters if free and nonfull lists are drained */
	if (vm_swap_full())
		swap_reclaim_full_clusters(si, false);

	if (order < PMD_ORDER) {
		/*
		 * Scan only one fragment cluster is good enough. Order 0
		 * allocation will surely success, and large allocation
		 * failure is not critical. Scanning one cluster still
		 * keeps the list rotated and reclaimed (for HAS_CACHE).
		 */
		found = alloc_swap_scan_list(si, &si->frag_clusters[order], order,
					     usage, false);
		if (found)
			goto done;
	}

	if (order)
		goto done;

	/* Order 0 stealing from higher order */
	for (int o = 1; o < SWAP_NR_ORDERS; o++) {
		/*
		 * Clusters here have at least one usable slots and can't fail order 0
		 * allocation, but reclaim may drop si->lock and race with another user.
		 */
		found = alloc_swap_scan_list(si, &si->frag_clusters[o],
					     0, usage, true);
		if (found)
			goto done;

		found = alloc_swap_scan_list(si, &si->nonfull_clusters[o],
					     0, usage, true);
		if (found)
			goto done;
	}
done:
	if (!(si->flags & SWP_SOLIDSTATE))
		spin_unlock(&si->global_cluster_lock);

	return found;
}

/* SWAP_USAGE_OFFLIST_BIT can only be set by this helper. */
static void del_from_avail_list(struct swap_info_struct *si, bool swapoff)
{
	unsigned long pages;

	spin_lock(&swap_avail_lock);

	if (swapoff) {
		/*
		 * Forcefully remove it. Clear the SWP_WRITEOK flags for
		 * swapoff here so it's synchronized by both si->lock and
		 * swap_avail_lock, to ensure the result can be seen by
		 * add_to_avail_list.
		 */
		lockdep_assert_held(&si->lock);
		si->flags &= ~SWP_WRITEOK;
		atomic_long_or(SWAP_USAGE_OFFLIST_BIT, &si->inuse_pages);
	} else {
		/*
		 * If not called by swapoff, take it off-list only if it's
		 * full and SWAP_USAGE_OFFLIST_BIT is not set (strictly
		 * si->inuse_pages == pages), any concurrent slot freeing,
		 * or device already removed from plist by someone else
		 * will make this return false.
		 */
		pages = si->pages;
		if (!atomic_long_try_cmpxchg(&si->inuse_pages, &pages,
					     pages | SWAP_USAGE_OFFLIST_BIT))
			goto skip;
	}

	plist_del(&si->avail_list, &swap_avail_head);

skip:
	spin_unlock(&swap_avail_lock);
}

/* SWAP_USAGE_OFFLIST_BIT can only be cleared by this helper. */
static void add_to_avail_list(struct swap_info_struct *si, bool swapon)
{
	long val;
	unsigned long pages;

	spin_lock(&swap_avail_lock);

	/* Corresponding to SWP_WRITEOK clearing in del_from_avail_list */
	if (swapon) {
		lockdep_assert_held(&si->lock);
		si->flags |= SWP_WRITEOK;
	} else {
		if (!(READ_ONCE(si->flags) & SWP_WRITEOK))
			goto skip;
	}

	if (!(atomic_long_read(&si->inuse_pages) & SWAP_USAGE_OFFLIST_BIT))
		goto skip;

	val = atomic_long_fetch_and_relaxed(~SWAP_USAGE_OFFLIST_BIT, &si->inuse_pages);

	/*
	 * When device is full and device is on the plist, only one updater will
	 * see (inuse_pages == si->pages) and will call del_from_avail_list. If
	 * that updater happen to be here, just skip adding.
	 */
	pages = si->pages;
	if (val == pages) {
		/* Just like the cmpxchg in del_from_avail_list */
		if (atomic_long_try_cmpxchg(&si->inuse_pages, &pages,
					    pages | SWAP_USAGE_OFFLIST_BIT))
			goto skip;
	}

	plist_add(&si->avail_list, &swap_avail_head);

skip:
	spin_unlock(&swap_avail_lock);
}

/*
 * swap_usage_add / swap_usage_sub of each slot are serialized by ci->lock
 * within each cluster, so the total contribution to the global counter should
 * always be positive and cannot exceed the total number of usable slots.
 */
static bool swap_usage_add(struct swap_info_struct *si, unsigned int nr_entries)
{
	long val = atomic_long_add_return_relaxed(nr_entries, &si->inuse_pages);

	/*
	 * If device is full, and SWAP_USAGE_OFFLIST_BIT is not set,
	 * remove it from the plist.
	 */
	if (unlikely(val == si->pages)) {
		del_from_avail_list(si, false);
		return true;
	}

	return false;
}

static void swap_usage_sub(struct swap_info_struct *si, unsigned int nr_entries)
{
	long val = atomic_long_sub_return_relaxed(nr_entries, &si->inuse_pages);

	/*
	 * If device is not full, and SWAP_USAGE_OFFLIST_BIT is set,
	 * add it to the plist.
	 */
	if (unlikely(val & SWAP_USAGE_OFFLIST_BIT))
		add_to_avail_list(si, false);
}

static void swap_range_alloc(struct swap_info_struct *si,
			     unsigned int nr_entries)
{
	if (swap_usage_add(si, nr_entries)) {
		if (vm_swap_full())
			schedule_work(&si->reclaim_work);
	}
	atomic_long_sub(nr_entries, &nr_swap_pages);
}

static void swap_range_free(struct swap_info_struct *si, unsigned long offset,
			    unsigned int nr_entries)
{
	unsigned long end = offset + nr_entries - 1;
	void (*swap_slot_free_notify)(struct block_device *, unsigned long);

	if (si->flags & SWP_BLKDEV)
		swap_slot_free_notify =
			si->bdev->bd_disk->fops->swap_slot_free_notify;
	else
		swap_slot_free_notify = NULL;
	while (offset <= end) {
		arch_swap_invalidate_page(si->type, offset);
		if (swap_slot_free_notify)
			swap_slot_free_notify(si->bdev, offset);
		offset++;
	}

	/*
	 * Make sure that try_to_unuse() observes si->inuse_pages reaching 0
	 * only after the above cleanups are done.
	 */
	smp_wmb();
	atomic_long_add(nr_entries, &nr_swap_pages);
	swap_usage_sub(si, nr_entries);
}

static bool get_swap_device_info(struct swap_info_struct *si)
{
	if (!percpu_ref_tryget_live(&si->users))
		return false;
	/*
	 * Guarantee the si->users are checked before accessing other
	 * fields of swap_info_struct, and si->flags (SWP_WRITEOK) is
	 * up to dated.
	 *
	 * Paired with the spin_unlock() after setup_swap_info() in
	 * enable_swap_info(), and smp_wmb() in swapoff.
	 */
	smp_rmb();
	return true;
}

/*
 * Fast path try to get swap entries with specified order from current
 * CPU's swap entry pool (a cluster).
 */
static bool swap_alloc_fast(swp_slot_t *slot, int order)
{
	struct swap_cluster_info *ci;
	struct swap_info_struct *si;
	unsigned int offset, found = SWAP_SLOT_INVALID;

	/*
	 * Once allocated, swap_info_struct will never be completely freed,
	 * so checking it's liveness by get_swap_device_info is enough.
	 */
	si = this_cpu_read(percpu_swap_cluster.si[order]);
	offset = this_cpu_read(percpu_swap_cluster.offset[order]);
	if (!si || !offset || !get_swap_device_info(si))
		return false;

	ci = swap_cluster_lock(si, offset);
	if (cluster_is_usable(ci, order)) {
		if (cluster_is_empty(ci))
			offset = cluster_offset(si, ci);
		found = alloc_swap_scan_cluster(si, ci, offset, order,
			SWAP_MAP_ALLOCATED);
		if (found)
			*slot = swp_slot(si->type, found);
	} else {
		swap_cluster_unlock(ci);
	}

	swap_slot_put_swap_info(si);
	return !!found;
}

/* Rotate the device and switch to a new cluster */
static void swap_alloc_slow(swp_slot_t *slot, int order)
{
	unsigned long offset;
	struct swap_info_struct *si, *next;

	spin_lock(&swap_avail_lock);
start_over:
	plist_for_each_entry_safe(si, next, &swap_avail_head, avail_list) {
		/* Rotate the device and switch to a new cluster */
		plist_requeue(&si->avail_list, &swap_avail_head);
		spin_unlock(&swap_avail_lock);
		if (get_swap_device_info(si)) {
			offset = cluster_alloc_swap_slot(si, order, SWAP_MAP_ALLOCATED);
			swap_slot_put_swap_info(si);
			if (offset) {
				*slot = swp_slot(si->type, offset);
				return;
			}
			if (order)
				return;
		}

		spin_lock(&swap_avail_lock);
		/*
		 * if we got here, it's likely that si was almost full before,
		 * multiple callers probably all tried to get a page from the
		 * same si and it filled up before we could get one; or, the si
		 * filled up between us dropping swap_avail_lock.
		 * Since we dropped the swap_avail_lock, the swap_avail_list
		 * may have been modified; so if next is still in the
		 * swap_avail_head list then try it, otherwise start over if we
		 * have not gotten any slots.
		 */
		if (plist_node_empty(&next->avail_list))
			goto start_over;
	}
	spin_unlock(&swap_avail_lock);
}

/*
 * Discard pending clusters in a synchronized way when under high pressure.
 * Return: true if any cluster is discarded.
 */
static bool swap_sync_discard(void)
{
	bool ret = false;
	struct swap_info_struct *si, *next;

	spin_lock(&swap_lock);
start_over:
	plist_for_each_entry_safe(si, next, &swap_active_head, list) {
		spin_unlock(&swap_lock);
		if (get_swap_device_info(si)) {
			if (si->flags & SWP_PAGE_DISCARD)
				ret = swap_do_scheduled_discard(si);
			swap_slot_put_swap_info(si);
		}
		if (ret)
			return true;

		spin_lock(&swap_lock);
		if (plist_node_empty(&next->list))
			goto start_over;
	}
	spin_unlock(&swap_lock);

	return false;
}

int swap_slot_alloc(swp_slot_t *slot, unsigned int order)
{
	unsigned int size = 1 << order;

	if (order) {
		/*
		 * Reject large allocation when THP_SWAP is disabled,
		 * the caller should split the folio and try again.
		 */
		if (!IS_ENABLED(CONFIG_THP_SWAP))
			return -EAGAIN;

		/*
		 * Allocation size should never exceed cluster size
		 * (HPAGE_PMD_SIZE).
		 */
		if (size > SWAPFILE_CLUSTER) {
			VM_WARN_ON_ONCE(1);
			return -EINVAL;
		}
	}

again:
	local_lock(&percpu_swap_cluster.lock);
	if (!swap_alloc_fast(slot, order))
		swap_alloc_slow(slot, order);
	local_unlock(&percpu_swap_cluster.lock);

	if (unlikely(!order && !slot->val)) {
		if (swap_sync_discard())
			goto again;
	}

	if (!slot->val)
		return -ENOMEM;

	return 0;
}

static struct swap_info_struct *_swap_info_get(swp_slot_t slot)
{
	struct swap_info_struct *si;
	unsigned long offset;

	if (!slot.val)
		goto out;
	si = swap_slot_to_info(slot);
	if (!si)
		goto bad_nofile;
	if (data_race(!(si->flags & SWP_USED)))
		goto bad_device;
	offset = swp_slot_offset(slot);
	if (offset >= si->max)
		goto bad_offset;
	if (data_race(!test_bit(offset, si->swap_map)))
		goto bad_free;
	return si;

bad_free:
	pr_err("%s: %s%08lx\n", __func__, Unused_offset, slot.val);
	goto out;
bad_offset:
	pr_err("%s: %s%08lx\n", __func__, Bad_offset, slot.val);
	goto out;
bad_device:
	pr_err("%s: %s%08lx\n", __func__, Unused_file, slot.val);
	goto out;
bad_nofile:
	pr_err("%s: %s%08lx\n", __func__, Bad_file, slot.val);
out:
	return NULL;
}

/*
 * When we get a swap entry, if there aren't some other ways to
 * prevent swapoff, such as the folio in swap cache is locked, RCU
 * reader side is locked, etc., the swap entry may become invalid
 * because of swapoff.  Then, we need to enclose all swap related
 * functions with swap_slot_tryget_swap_info() and
 * swap_slot_put_swap_info(), unless the swap functions call
 * swap_slot_(tryget|put)_swap_info by themselves.
 *
 * RCU reader side lock (including any spinlock) is sufficient to
 * prevent swapoff, because synchronize_rcu() is called in swapoff()
 * before freeing data structures.
 *
 * Check whether swap entry is valid in the swap device.  If so,
 * return pointer to swap_info_struct, and keep the swap entry valid
 * via preventing the swap device from being swapoff, until
 * swap_slot_put_swap_info() is called.  Otherwise return NULL.
 *
 * Notice that swapoff or swapoff+swapon can still happen before the
 * percpu_ref_tryget_live() in swap_slot_tryget_swap_info() or after the
 * percpu_ref_put() in swap_slot_put_swap_info() if there isn't any other way
 * to prevent swapoff.  The caller must be prepared for that.  For
 * example, the following situation is possible.
 *
 *   CPU1				CPU2
 *   do_swap_page()
 *     ...				swapoff+swapon
 *     __read_swap_cache_async()
 *       swapcache_prepare()
 *         __swap_duplicate()
 *           // check swap_map
 *     // verify PTE not changed
 *
 * In __swap_duplicate(), the swap_map need to be checked before
 * changing partly because the specified swap entry may be for another
 * swap device which has been swapoff.  And in do_swap_page(), after
 * the page is read from the swap device, the PTE is verified not
 * changed with the page table locked to check whether the swap device
 * has been swapoff or swapoff+swapon.
 */
struct swap_info_struct *swap_slot_tryget_swap_info(swp_slot_t slot)
{
	struct swap_info_struct *si;
	unsigned long offset;

	if (!slot.val)
		goto out;
	si = swap_slot_to_info(slot);
	if (!si)
		goto bad_nofile;
	if (!get_swap_device_info(si))
		goto out;
	offset = swp_slot_offset(slot);
	if (offset >= si->max)
		goto put_out;

	return si;
bad_nofile:
	pr_err("%s: %s%08lx\n", __func__, Bad_file, slot.val);
out:
	return NULL;
put_out:
	pr_err("%s: %s%08lx\n", __func__, Bad_offset, slot.val);
	percpu_ref_put(&si->users);
	return NULL;
}

static bool swap_slots_put_map(struct swap_info_struct *si,
				 swp_slot_t slot, int nr)
{
	unsigned long offset = swp_slot_offset(slot);
	struct swap_cluster_info *ci;

	ci = swap_cluster_lock(si, offset);
	vswap_rmap_set(ci, slot, 0, nr);
	swap_slots_free(si, ci, slot, nr);
	swap_cluster_unlock(ci);

	return true;
}

static inline bool __maybe_unused swap_is_last_ref(unsigned char count)
{
	return count == SWAP_MAP_ALLOCATED;
}

/*
 * Drop the last ref of swap entries, caller have to ensure all entries
 * belong to the same cgroup and cluster.
 */
static void swap_slots_free(struct swap_info_struct *si,
			      struct swap_cluster_info *ci,
			      swp_slot_t slot, unsigned int nr_pages)
{
	unsigned long offset = swp_slot_offset(slot);
	unsigned long end = offset + nr_pages;

	/* It should never free entries across different clusters */
	VM_BUG_ON(ci != __swap_offset_to_cluster(si, offset + nr_pages - 1));
	VM_BUG_ON(cluster_is_empty(ci));
	VM_BUG_ON(ci->count < nr_pages);

	ci->count -= nr_pages;
	VM_BUG_ON(find_next_zero_bit(si->swap_map, end, offset) < end);
	bitmap_clear(si->swap_map, offset, nr_pages);

	swap_range_free(si, offset, nr_pages);

	if (!ci->count)
		free_cluster(si, ci);
	else
		partial_free_cluster(si, ci);
}

/*
 * Caller has made sure that the swap device corresponding to entry
 * is still around or has not been recycled.
 */
void swap_slot_free_nr(swp_slot_t slot, int nr_pages)
{
	int nr;
	struct swap_info_struct *sis;
	unsigned long offset = swp_slot_offset(slot);

	sis = _swap_info_get(slot);
	if (!sis)
		return;

	while (nr_pages) {
		nr = min_t(int, nr_pages, SWAPFILE_CLUSTER - offset % SWAPFILE_CLUSTER);
		swap_slots_put_map(sis, swp_slot(sis->type, offset), nr);
		offset += nr;
		nr_pages -= nr;
	}
}

static bool folio_swapcache_freeable(struct folio *folio)
{
	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);

	if (!folio_test_swapcache(folio))
		return false;
	if (folio_test_writeback(folio))
		return false;

	/*
	 * Once hibernation has begun to create its image of memory,
	 * there's a danger that one of the calls to folio_free_swap()
	 * - most probably a call from __try_to_reclaim_swap() while
	 * hibernation is allocating its own swap pages for the image,
	 * but conceivably even a call from memory reclaim - will free
	 * the swap from a folio which has already been recorded in the
	 * image as a clean swapcache folio, and then reuse its swap for
	 * another page of the image.  On waking from hibernation, the
	 * original folio might be freed under memory pressure, then
	 * later read back in from swap, now with the wrong data.
	 *
	 * Hibernation suspends storage while it is writing the image
	 * to disk so check that here.
	 */
	if (pm_suspended_storage())
		return false;

	return true;
}

/**
 * folio_free_swap() - Free the swap space used for this folio.
 * @folio: The folio to remove.
 *
 * If swap is getting full, or if there are no more mappings of this folio,
 * then call folio_free_swap to free its swap space.
 *
 * Return: true if we were able to release the swap space.
 */
bool folio_free_swap(struct folio *folio)
{
	if (!folio_swapcache_freeable(folio))
		return false;
	if (folio_swapped(folio))
		return false;

	swap_cache_del_folio(folio);
	folio_set_dirty(folio);
	return true;
}

#ifdef CONFIG_HIBERNATION

swp_slot_t swap_slot_alloc_of_type(int type)
{
	struct swap_info_struct *si = swap_type_to_info(type);
	unsigned long offset;
	swp_slot_t slot = {0};

	if (!si)
		goto fail;

	/* This is called for allocating swap entry, not cache */
	if (get_swap_device_info(si)) {
		if (si->flags & SWP_WRITEOK) {
			/*
			 * Grab the local lock to be complaint
			 * with swap table allocation.
			 */
			local_lock(&percpu_swap_cluster.lock);
			offset = cluster_alloc_swap_slot(si, 0, 1);
			local_unlock(&percpu_swap_cluster.lock);
			if (offset)
				slot = swp_slot(si->type, offset);
		}
		swap_slot_put_swap_info(si);
	}
fail:
	return slot;
}

/*
 * Find the swap type that corresponds to given device (if any).
 *
 * @offset - number of the PAGE_SIZE-sized block of the device, starting
 * from 0, in which the swap header is expected to be located.
 *
 * This is needed for the suspend to disk (aka swsusp).
 */
int swap_type_of(dev_t device, sector_t offset)
{
	int type;

	if (!device)
		return -1;

	spin_lock(&swap_lock);
	for (type = 0; type < nr_swapfiles; type++) {
		struct swap_info_struct *sis = swap_info[type];

		if (!(sis->flags & SWP_WRITEOK))
			continue;

		if (device == sis->bdev->bd_dev) {
			struct swap_extent *se = first_se(sis);

			if (se->start_block == offset) {
				spin_unlock(&swap_lock);
				return type;
			}
		}
	}
	spin_unlock(&swap_lock);
	return -ENODEV;
}

int find_first_swap(dev_t *device)
{
	int type;

	spin_lock(&swap_lock);
	for (type = 0; type < nr_swapfiles; type++) {
		struct swap_info_struct *sis = swap_info[type];

		if (!(sis->flags & SWP_WRITEOK))
			continue;
		*device = sis->bdev->bd_dev;
		spin_unlock(&swap_lock);
		return type;
	}
	spin_unlock(&swap_lock);
	return -ENODEV;
}

/*
 * Get the (PAGE_SIZE) block corresponding to given offset on the swapdev
 * corresponding to given index in swap_info (swap type).
 */
sector_t swapdev_block(int type, pgoff_t offset)
{
	struct swap_info_struct *si = swap_type_to_info(type);
	struct swap_extent *se;

	if (!si || !(si->flags & SWP_WRITEOK))
		return 0;
	se = offset_to_swap_extent(si, offset);
	return se->start_block + (offset - se->start_page);
}

/*
 * Return either the total number of swap pages of given type, or the number
 * of free pages of that type (depending on @free)
 *
 * This is needed for software suspend
 */
unsigned int count_swap_pages(int type, int free)
{
	unsigned int n = 0;

	spin_lock(&swap_lock);
	if ((unsigned int)type < nr_swapfiles) {
		struct swap_info_struct *sis = swap_info[type];

		spin_lock(&sis->lock);
		if (sis->flags & SWP_WRITEOK) {
			n = sis->pages;
			if (free)
				n -= swap_usage_in_pages(sis);
		}
		spin_unlock(&sis->lock);
	}
	spin_unlock(&swap_lock);
	return n;
}
#endif /* CONFIG_HIBERNATION */

static bool swap_slot_allocated(struct swap_info_struct *si,
		unsigned long offset)
{
	return test_bit(offset, si->swap_map);
}

/*
 * Scan swap_map from current position to next entry still in use.
 * Return 0 if there are no inuse entries after prev till end of
 * the map.
 */
static unsigned int find_next_to_unuse(struct swap_info_struct *si,
					unsigned int prev)
{
	unsigned int i;

	/*
	 * No need for swap_lock here: we're just looking
	 * for whether an entry is in use, not modifying it; false
	 * hits are okay, and sys_swapoff() has already prevented new
	 * allocations from this area (while holding swap_lock).
	 */
	for (i = prev + 1; i < si->max; i++) {
		if (swap_slot_allocated(si, i))
			break;
		if ((i % LATENCY_LIMIT) == 0)
			cond_resched();
	}

	if (i == si->max)
		i = 0;

	return i;
}

#define	for_each_allocated_offset(si, offset)	\
	while (swap_usage_in_pages(si) && \
		!signal_pending(current) && \
		(offset = find_next_to_unuse(si, offset)) != 0)

static struct folio *pagein(swp_entry_t entry, struct swap_iocb **splug,
		struct mempolicy *mpol)
{
	bool folio_was_allocated;
	struct folio *folio = __read_swap_cache_async(entry, GFP_KERNEL, mpol,
			NO_INTERLEAVE_INDEX, &folio_was_allocated, false);

	if (folio_was_allocated)
		swap_read_folio(folio, splug);
	return folio;
}

static int try_to_unuse(unsigned int type)
{
	struct swap_info_struct *si = swap_info[type];
	struct swap_iocb *splug = NULL;
	struct mempolicy *mpol;
	struct blk_plug plug;
	unsigned long offset;
	struct folio *folio;
	swp_entry_t entry;
	swp_slot_t slot;
	int ret = 0;

	if (!swap_usage_in_pages(si))
		goto success;

	mpol = get_task_policy(current);
	blk_start_plug(&plug);

	/* first round - submit the reads */
	offset = 0;
	for_each_allocated_offset(si, offset) {
		slot = swp_slot(type, offset);
		entry = swp_slot_to_swp_entry(slot);
		if (!entry.val)
			continue;

		folio = pagein(entry, &splug, mpol);
		if (folio)
			folio_put(folio);
	}
	blk_finish_plug(&plug);
	swap_read_unplug(splug);
	splug = NULL;
	lru_add_drain();

	/* second round - updating the virtual swap slots' backing state */
	offset = 0;
	for_each_allocated_offset(si, offset) {
		slot = swp_slot(type, offset);
retry:
		entry = swp_slot_to_swp_entry(slot);
		if (!entry.val) {
			if (!swap_slot_allocated(si, offset))
				continue;

			if (signal_pending(current)) {
				ret = -EINTR;
				goto out;
			}

			/* we might be racing with zswap writeback or disk swapout */
			schedule_timeout_uninterruptible(1);
			goto retry;
		}

		/* try to allocate swap cache folio */
		folio = pagein(entry, &splug, mpol);
		if (!folio) {
			if (!swp_slot_to_swp_entry(swp_slot(type, offset)).val)
				continue;

			ret = -ENOMEM;
			pr_err("swapoff: unable to allocate swap cache folio for %lu\n",
						entry.val);
			goto out;
		}

		folio_lock(folio);
		/*
		 * We need to check if the folio is still in swap cache, and is still
		 * backed by the physical swap slot we are trying to release.
		 *
		 * We can, for instance, race with zswap writeback, obtaining the
		 * temporary folio it allocated for decompression and writeback, which
		 * would be promptly deleted from swap cache. By the time we lock that
		 * folio, it might have already contained stale data.
		 *
		 * Concurrent swap operations might have also come in before we
		 * reobtain the folio's lock, deleting the folio from swap cache,
		 * invalidating the virtual swap slot, then swapping out the folio
		 * again to a different swap backends.
		 *
		 * In all of these cases, we must retry the physical -> virtual lookup.
		 */
		if (!folio_matches_swap_slot(folio, entry, slot)) {
			folio_unlock(folio);
			folio_put(folio);
			if (signal_pending(current)) {
				ret = -EINTR;
				goto out;
			}
			schedule_timeout_uninterruptible(1);
			goto retry;
		}

		folio_wait_writeback(folio);
		vswap_store_folio(entry, folio);
		folio_mark_dirty(folio);
		folio_unlock(folio);
		folio_put(folio);
	}

	/* concurrent swappers might still be releasing physical swap slots... */
	while (swap_usage_in_pages(si)) {
		if (signal_pending(current)) {
			ret = -EINTR;
			goto out;
		}
		schedule_timeout_uninterruptible(1);
	}

out:
	swap_read_unplug(splug);
	if (ret)
		return ret;

success:
	/*
	 * Make sure that further cleanups after try_to_unuse() returns happen
	 * after swap_range_free() reduces si->inuse_pages to 0.
	 */
	smp_mb();
	return 0;
}

/*
 * After a successful try_to_unuse, if no swap is now in use, we know
 * we can empty the mmlist.  swap_lock must be held on entry and exit.
 * Note that mmlist_lock nests inside swap_lock, and an mm must be
 * added to the mmlist just after page_duplicate - before would be racy.
 */
static void drain_mmlist(void)
{
	struct list_head *p, *next;
	unsigned int type;

	for (type = 0; type < nr_swapfiles; type++)
		if (swap_usage_in_pages(swap_info[type]))
			return;
	spin_lock(&mmlist_lock);
	list_for_each_safe(p, next, &init_mm.mmlist)
		list_del_init(p);
	spin_unlock(&mmlist_lock);
}

/*
 * Free all of a swapdev's extent information
 */
static void destroy_swap_extents(struct swap_info_struct *sis)
{
	while (!RB_EMPTY_ROOT(&sis->swap_extent_root)) {
		struct rb_node *rb = sis->swap_extent_root.rb_node;
		struct swap_extent *se = rb_entry(rb, struct swap_extent, rb_node);

		rb_erase(rb, &sis->swap_extent_root);
		kfree(se);
	}

	if (sis->flags & SWP_ACTIVATED) {
		struct file *swap_file = sis->swap_file;
		struct address_space *mapping = swap_file->f_mapping;

		sis->flags &= ~SWP_ACTIVATED;
		if (mapping->a_ops->swap_deactivate)
			mapping->a_ops->swap_deactivate(swap_file);
	}
}

/*
 * Add a block range (and the corresponding page range) into this swapdev's
 * extent tree.
 *
 * This function rather assumes that it is called in ascending page order.
 */
int
add_swap_extent(struct swap_info_struct *sis, unsigned long start_page,
		unsigned long nr_pages, sector_t start_block)
{
	struct rb_node **link = &sis->swap_extent_root.rb_node, *parent = NULL;
	struct swap_extent *se;
	struct swap_extent *new_se;

	/*
	 * place the new node at the right most since the
	 * function is called in ascending page order.
	 */
	while (*link) {
		parent = *link;
		link = &parent->rb_right;
	}

	if (parent) {
		se = rb_entry(parent, struct swap_extent, rb_node);
		BUG_ON(se->start_page + se->nr_pages != start_page);
		if (se->start_block + se->nr_pages == start_block) {
			/* Merge it */
			se->nr_pages += nr_pages;
			return 0;
		}
	}

	/* No merge, insert a new extent. */
	new_se = kmalloc(sizeof(*se), GFP_KERNEL);
	if (new_se == NULL)
		return -ENOMEM;
	new_se->start_page = start_page;
	new_se->nr_pages = nr_pages;
	new_se->start_block = start_block;

	rb_link_node(&new_se->rb_node, parent, link);
	rb_insert_color(&new_se->rb_node, &sis->swap_extent_root);
	return 1;
}
EXPORT_SYMBOL_GPL(add_swap_extent);

/*
 * A `swap extent' is a simple thing which maps a contiguous range of pages
 * onto a contiguous range of disk blocks.  A rbtree of swap extents is
 * built at swapon time and is then used at swap_writepage/swap_read_folio
 * time for locating where on disk a page belongs.
 *
 * If the swapfile is an S_ISBLK block device, a single extent is installed.
 * This is done so that the main operating code can treat S_ISBLK and S_ISREG
 * swap files identically.
 *
 * Whether the swapdev is an S_ISREG file or an S_ISBLK blockdev, the swap
 * extent rbtree operates in PAGE_SIZE disk blocks.  Both S_ISREG and S_ISBLK
 * swapfiles are handled *identically* after swapon time.
 *
 * For S_ISREG swapfiles, setup_swap_extents() will walk all the file's blocks
 * and will parse them into a rbtree, in PAGE_SIZE chunks.  If some stray
 * blocks are found which do not fall within the PAGE_SIZE alignment
 * requirements, they are simply tossed out - we will never use those blocks
 * for swapping.
 *
 * For all swap devices we set S_SWAPFILE across the life of the swapon.  This
 * prevents users from writing to the swap device, which will corrupt memory.
 *
 * The amount of disk space which a single swap extent represents varies.
 * Typically it is in the 1-4 megabyte range.  So we can have hundreds of
 * extents in the rbtree. - akpm.
 */
static int setup_swap_extents(struct swap_info_struct *sis, sector_t *span)
{
	struct file *swap_file = sis->swap_file;
	struct address_space *mapping = swap_file->f_mapping;
	struct inode *inode = mapping->host;
	int ret;

	if (S_ISBLK(inode->i_mode)) {
		ret = add_swap_extent(sis, 0, sis->max, 0);
		*span = sis->pages;
		return ret;
	}

	if (mapping->a_ops->swap_activate) {
		ret = mapping->a_ops->swap_activate(sis, swap_file, span);
		if (ret < 0)
			return ret;
		sis->flags |= SWP_ACTIVATED;
		if ((sis->flags & SWP_FS_OPS) &&
		    sio_pool_init() != 0) {
			destroy_swap_extents(sis);
			return -ENOMEM;
		}
		return ret;
	}

	return generic_swapfile_activate(sis, swap_file, span);
}

static void setup_swap_info(struct swap_info_struct *si, int prio,
			    unsigned long *swap_map,
			    struct swap_cluster_info *cluster_info)
{
	si->prio = prio;
	/*
	 * the plist prio is negated because plist ordering is
	 * low-to-high, while swap ordering is high-to-low
	 */
	si->list.prio = -si->prio;
	si->avail_list.prio = -si->prio;
	si->swap_map = swap_map;
	si->cluster_info = cluster_info;
}

static void _enable_swap_info(struct swap_info_struct *si)
{
	atomic_long_add(si->pages, &nr_swap_pages);
	total_swap_pages += si->pages;

	assert_spin_locked(&swap_lock);

	plist_add(&si->list, &swap_active_head);

	/* Add back to available list */
	add_to_avail_list(si, true);
}

static void enable_swap_info(struct swap_info_struct *si, int prio,
				unsigned long *swap_map,
				struct swap_cluster_info *cluster_info)
{
	spin_lock(&swap_lock);
	spin_lock(&si->lock);
	setup_swap_info(si, prio, swap_map, cluster_info);
	spin_unlock(&si->lock);
	spin_unlock(&swap_lock);
	/*
	 * Finished initializing swap device, now it's safe to reference it.
	 */
	percpu_ref_resurrect(&si->users);
	spin_lock(&swap_lock);
	spin_lock(&si->lock);
	_enable_swap_info(si);
	spin_unlock(&si->lock);
	spin_unlock(&swap_lock);
}

static void reinsert_swap_info(struct swap_info_struct *si)
{
	spin_lock(&swap_lock);
	spin_lock(&si->lock);
	setup_swap_info(si, si->prio, si->swap_map, si->cluster_info);
	_enable_swap_info(si);
	spin_unlock(&si->lock);
	spin_unlock(&swap_lock);
}

/*
 * Called after clearing SWP_WRITEOK, ensures cluster_alloc_range
 * see the updated flags, so there will be no more allocations.
 */
static void wait_for_allocation(struct swap_info_struct *si)
{
	unsigned long offset;
	unsigned long end = ALIGN(si->max, SWAPFILE_CLUSTER);
	struct swap_cluster_info *ci;

	BUG_ON(si->flags & SWP_WRITEOK);

	for (offset = 0; offset < end; offset += SWAPFILE_CLUSTER) {
		ci = swap_cluster_lock(si, offset);
		swap_cluster_unlock(ci);
	}
}

static void free_cluster_info(struct swap_cluster_info *cluster_info,
			      unsigned long maxpages)
{
	struct swap_cluster_info *ci;
	int i, nr_clusters = DIV_ROUND_UP(maxpages, SWAPFILE_CLUSTER);

	if (!cluster_info)
		return;
	for (i = 0; i < nr_clusters; i++) {
		ci = cluster_info + i;
		/* Cluster with bad marks count will have a remaining table */
		spin_lock(&ci->lock);
		if (rcu_dereference_protected(ci->table, true)) {
			ci->count = 0;
			swap_cluster_free_table(ci);
		}
		spin_unlock(&ci->lock);
	}
	kvfree(cluster_info);
}

/*
 * Called after swap device's reference count is dead, so
 * neither scan nor allocation will use it.
 */
static void flush_percpu_swap_cluster(struct swap_info_struct *si)
{
	int cpu, i;
	struct swap_info_struct **pcp_si;

	for_each_possible_cpu(cpu) {
		pcp_si = per_cpu_ptr(percpu_swap_cluster.si, cpu);
		/*
		 * Invalidate the percpu swap cluster cache, si->users
		 * is dead, so no new user will point to it, just flush
		 * any existing user.
		 */
		for (i = 0; i < SWAP_NR_ORDERS; i++)
			cmpxchg(&pcp_si[i], si, NULL);
	}
}


SYSCALL_DEFINE1(swapoff, const char __user *, specialfile)
{
	struct swap_info_struct *p = NULL;
	unsigned long *swap_map;
	unsigned long *bad_map;
	struct swap_cluster_info *cluster_info;
	struct file *swap_file, *victim;
	struct address_space *mapping;
	struct inode *inode;
	struct filename *pathname;
	unsigned int maxpages;
	int err, found = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	BUG_ON(!current->mm);

	pathname = getname(specialfile);
	if (IS_ERR(pathname))
		return PTR_ERR(pathname);

	victim = file_open_name(pathname, O_RDWR|O_LARGEFILE, 0);
	err = PTR_ERR(victim);
	if (IS_ERR(victim))
		goto out;

	mapping = victim->f_mapping;
	spin_lock(&swap_lock);
	plist_for_each_entry(p, &swap_active_head, list) {
		if (p->flags & SWP_WRITEOK) {
			if (p->swap_file->f_mapping == mapping) {
				found = 1;
				break;
			}
		}
	}
	if (!found) {
		err = -EINVAL;
		spin_unlock(&swap_lock);
		goto out_dput;
	}
	if (!security_vm_enough_memory_mm(current->mm, p->pages))
		vm_unacct_memory(p->pages);
	else {
		err = -ENOMEM;
		spin_unlock(&swap_lock);
		goto out_dput;
	}
	spin_lock(&p->lock);
	del_from_avail_list(p, true);
	plist_del(&p->list, &swap_active_head);
	atomic_long_sub(p->pages, &nr_swap_pages);
	total_swap_pages -= p->pages;
	spin_unlock(&p->lock);
	spin_unlock(&swap_lock);

	wait_for_allocation(p);

	set_current_oom_origin();
	err = try_to_unuse(p->type);
	clear_current_oom_origin();

	if (err) {
		/* re-insert swap space back into swap_list */
		reinsert_swap_info(p);
		goto out_dput;
	}

	/*
	 * Wait for swap operations protected by swap_slot_(tryget|put)_swap_info()
	 * to complete.  Because of synchronize_rcu() here, all swap
	 * operations protected by RCU reader side lock (including any
	 * spinlock) will be waited too.  This makes it easy to
	 * prevent folio_test_swapcache() and the following swap cache
	 * operations from racing with swapoff.
	 */
	percpu_ref_kill(&p->users);
	synchronize_rcu();
	wait_for_completion(&p->comp);

	flush_work(&p->discard_work);
	flush_work(&p->reclaim_work);
	flush_percpu_swap_cluster(p);

	destroy_swap_extents(p);

	if (!(p->flags & SWP_SOLIDSTATE))
		atomic_dec(&nr_rotate_swap);

	mutex_lock(&swapon_mutex);
	spin_lock(&swap_lock);
	spin_lock(&p->lock);
	drain_mmlist();

	swap_file = p->swap_file;
	p->swap_file = NULL;
	swap_map = p->swap_map;
	p->swap_map = NULL;
	bad_map = p->bad_map;
	p->bad_map = NULL;
	maxpages = p->max;
	cluster_info = p->cluster_info;
	p->max = 0;
	p->cluster_info = NULL;
	spin_unlock(&p->lock);
	spin_unlock(&swap_lock);
	arch_swap_invalidate_area(p->type);
	mutex_unlock(&swapon_mutex);
	kfree(p->global_cluster);
	p->global_cluster = NULL;
	kvfree(swap_map);
	kvfree(bad_map);
	free_cluster_info(cluster_info, maxpages);

	inode = mapping->host;

	inode_lock(inode);
	inode->i_flags &= ~S_SWAPFILE;
	inode_unlock(inode);
	filp_close(swap_file, NULL);

	/*
	 * Clear the SWP_USED flag after all resources are freed so that swapon
	 * can reuse this swap_info in alloc_swap_info() safely.  It is ok to
	 * not hold p->lock after we cleared its SWP_WRITEOK.
	 */
	spin_lock(&swap_lock);
	p->flags = 0;
	spin_unlock(&swap_lock);

	err = 0;
	atomic_inc(&proc_poll_event);
	wake_up_interruptible(&proc_poll_wait);

out_dput:
	filp_close(victim, NULL);
out:
	putname(pathname);
	return err;
}

#ifdef CONFIG_PROC_FS
static __poll_t swaps_poll(struct file *file, poll_table *wait)
{
	struct seq_file *seq = file->private_data;

	poll_wait(file, &proc_poll_wait, wait);

	if (seq->poll_event != atomic_read(&proc_poll_event)) {
		seq->poll_event = atomic_read(&proc_poll_event);
		return EPOLLIN | EPOLLRDNORM | EPOLLERR | EPOLLPRI;
	}

	return EPOLLIN | EPOLLRDNORM;
}

/* iterator */
static void *swap_start(struct seq_file *swap, loff_t *pos)
{
	struct swap_info_struct *si;
	int type;
	loff_t l = *pos;

	mutex_lock(&swapon_mutex);

	if (!l)
		return SEQ_START_TOKEN;

	for (type = 0; (si = swap_type_to_info(type)); type++) {
		if (!(si->flags & SWP_USED) || !si->swap_map)
			continue;
		if (!--l)
			return si;
	}

	return NULL;
}

static void *swap_next(struct seq_file *swap, void *v, loff_t *pos)
{
	struct swap_info_struct *si = v;
	int type;

	if (v == SEQ_START_TOKEN)
		type = 0;
	else
		type = si->type + 1;

	++(*pos);
	for (; (si = swap_type_to_info(type)); type++) {
		if (!(si->flags & SWP_USED) || !si->swap_map)
			continue;
		return si;
	}

	return NULL;
}

static void swap_stop(struct seq_file *swap, void *v)
{
	mutex_unlock(&swapon_mutex);
}

static int swap_show(struct seq_file *swap, void *v)
{
	struct swap_info_struct *si = v;
	struct file *file;
	int len;
	unsigned long bytes, inuse;

	if (si == SEQ_START_TOKEN) {
		seq_puts(swap, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
		return 0;
	}

	bytes = K(si->pages);
	inuse = K(swap_usage_in_pages(si));

	file = si->swap_file;
	len = seq_file_path(swap, file, " \t\n\\");
	seq_printf(swap, "%*s%s\t%lu\t%s%lu\t%s%d\n",
			len < 40 ? 40 - len : 1, " ",
			S_ISBLK(file_inode(file)->i_mode) ?
				"partition" : "file\t",
			bytes, bytes < 10000000 ? "\t" : "",
			inuse, inuse < 10000000 ? "\t" : "",
			si->prio);
	return 0;
}

static const struct seq_operations swaps_op = {
	.start =	swap_start,
	.next =		swap_next,
	.stop =		swap_stop,
	.show =		swap_show
};

static int swaps_open(struct inode *inode, struct file *file)
{
	struct seq_file *seq;
	int ret;

	ret = seq_open(file, &swaps_op);
	if (ret)
		return ret;

	seq = file->private_data;
	seq->poll_event = atomic_read(&proc_poll_event);
	return 0;
}

static const struct proc_ops swaps_proc_ops = {
	.proc_flags	= PROC_ENTRY_PERMANENT,
	.proc_open	= swaps_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= seq_release,
	.proc_poll	= swaps_poll,
};

static int __init procswaps_init(void)
{
	proc_create("swaps", 0, NULL, &swaps_proc_ops);
	return 0;
}
__initcall(procswaps_init);
#endif /* CONFIG_PROC_FS */

#ifdef MAX_SWAPFILES_CHECK
static int __init max_swapfiles_check(void)
{
	MAX_SWAPFILES_CHECK();
	return 0;
}
late_initcall(max_swapfiles_check);
#endif

static struct swap_info_struct *alloc_swap_info(void)
{
	struct swap_info_struct *p;
	struct swap_info_struct *defer = NULL;
	unsigned int type;

	p = kvzalloc(sizeof(struct swap_info_struct), GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);

	if (percpu_ref_init(&p->users, swap_users_ref_free,
			    PERCPU_REF_INIT_DEAD, GFP_KERNEL)) {
		kvfree(p);
		return ERR_PTR(-ENOMEM);
	}

	spin_lock(&swap_lock);
	for (type = 0; type < nr_swapfiles; type++) {
		if (!(swap_info[type]->flags & SWP_USED))
			break;
	}
	if (type >= MAX_SWAPFILES) {
		spin_unlock(&swap_lock);
		percpu_ref_exit(&p->users);
		kvfree(p);
		return ERR_PTR(-EPERM);
	}
	if (type >= nr_swapfiles) {
		p->type = type;
		/*
		 * Publish the swap_info_struct after initializing it.
		 * Note that kvzalloc() above zeroes all its fields.
		 */
		smp_store_release(&swap_info[type], p); /* rcu_assign_pointer() */
		nr_swapfiles++;
	} else {
		defer = p;
		p = swap_info[type];
		/*
		 * Do not memset this entry: a racing procfs swap_next()
		 * would be relying on p->type to remain valid.
		 */
	}
	p->swap_extent_root = RB_ROOT;
	plist_node_init(&p->list, 0);
	plist_node_init(&p->avail_list, 0);
	p->flags = SWP_USED;
	spin_unlock(&swap_lock);
	if (defer) {
		percpu_ref_exit(&defer->users);
		kvfree(defer);
	}
	spin_lock_init(&p->lock);
	spin_lock_init(&p->cont_lock);
	atomic_long_set(&p->inuse_pages, SWAP_USAGE_OFFLIST_BIT);
	init_completion(&p->comp);

	return p;
}

static int claim_swapfile(struct swap_info_struct *si, struct inode *inode)
{
	if (S_ISBLK(inode->i_mode)) {
		si->bdev = I_BDEV(inode);
		/*
		 * Zoned block devices contain zones that have a sequential
		 * write only restriction.  Hence zoned block devices are not
		 * suitable for swapping.  Disallow them here.
		 */
		if (bdev_is_zoned(si->bdev))
			return -EINVAL;
		si->flags |= SWP_BLKDEV;
	} else if (S_ISREG(inode->i_mode)) {
		si->bdev = inode->i_sb->s_bdev;
	}

	return 0;
}


/*
 * Find out how many pages are allowed for a single swap device. There
 * are two limiting factors:
 * 1) the number of bits for the swap offset in the swp_entry_t type, and
 * 2) the number of bits in the swap pte, as defined by the different
 * architectures.
 *
 * In order to find the largest possible bit mask, a swap entry with
 * swap type 0 and swap offset ~0UL is created, encoded to a swap pte,
 * decoded to a swp_entry_t again, and finally the swap offset is
 * extracted.
 *
 * This will mask all the bits from the initial ~0UL mask that can't
 * be encoded in either the swp_entry_t or the architecture definition
 * of a swap pte.
 */
unsigned long generic_max_swapfile_size(void)
{
	swp_entry_t entry = swp_entry(0, ~0UL);
	const pte_t pte = softleaf_to_pte(entry);

	/*
	 * Since the PTE can be an invalid softleaf entry (e.g. the none PTE),
	 * we need to do this manually.
	 */
	entry = __pte_to_swp_entry(pte);
	entry = swp_entry(__swp_type(entry), __swp_offset(entry));

	return swp_offset(entry) + 1;
}

/* Can be overridden by an architecture for additional checks. */
__weak unsigned long arch_max_swapfile_size(void)
{
	return generic_max_swapfile_size();
}

static unsigned long read_swap_header(struct swap_info_struct *si,
					union swap_header *swap_header,
					struct inode *inode)
{
	int i;
	unsigned long maxpages;
	unsigned long swapfilepages;
	unsigned long last_page;

	if (memcmp("SWAPSPACE2", swap_header->magic.magic, 10)) {
		pr_err("Unable to find swap-space signature\n");
		return 0;
	}

	/* swap partition endianness hack... */
	if (swab32(swap_header->info.version) == 1) {
		swab32s(&swap_header->info.version);
		swab32s(&swap_header->info.last_page);
		swab32s(&swap_header->info.nr_badpages);
		if (swap_header->info.nr_badpages > MAX_SWAP_BADPAGES)
			return 0;
		for (i = 0; i < swap_header->info.nr_badpages; i++)
			swab32s(&swap_header->info.badpages[i]);
	}
	/* Check the swap header's sub-version */
	if (swap_header->info.version != 1) {
		pr_warn("Unable to handle swap header version %d\n",
			swap_header->info.version);
		return 0;
	}

	maxpages = swapfile_maximum_size;
	last_page = swap_header->info.last_page;
	if (!last_page) {
		pr_warn("Empty swap-file\n");
		return 0;
	}
	if (last_page > maxpages) {
		pr_warn("Truncating oversized swap area, only using %luk out of %luk\n",
			K(maxpages), K(last_page));
	}
	if (maxpages > last_page) {
		maxpages = last_page + 1;
		/* p->max is an unsigned int: don't overflow it */
		if ((unsigned int)maxpages == 0)
			maxpages = UINT_MAX;
	}

	if (!maxpages)
		return 0;
	swapfilepages = i_size_read(inode) >> PAGE_SHIFT;
	if (swapfilepages && maxpages > swapfilepages) {
		pr_warn("Swap area shorter than signature indicates\n");
		return 0;
	}
	if (swap_header->info.nr_badpages && S_ISREG(inode->i_mode))
		return 0;
	if (swap_header->info.nr_badpages > MAX_SWAP_BADPAGES)
		return 0;

	return maxpages;
}

static int setup_swap_map(struct swap_info_struct *si,
			  union swap_header *swap_header,
			  unsigned long *swap_map,
			  unsigned long *bad_map,
			  unsigned long maxpages)
{
	unsigned long i;

	set_bit(0, bad_map); /* omit header page */

	for (i = 0; i < swap_header->info.nr_badpages; i++) {
		unsigned int page_nr = swap_header->info.badpages[i];
		if (page_nr == 0 || page_nr > swap_header->info.last_page)
			return -EINVAL;
		if (page_nr < maxpages) {
			set_bit(page_nr, bad_map);
			si->pages--;
		}
	}

	if (!si->pages) {
		pr_warn("Empty swap-file\n");
		return -EINVAL;
	}

	return 0;
}

static struct swap_cluster_info *setup_clusters(struct swap_info_struct *si,
						union swap_header *swap_header,
						unsigned long maxpages)
{
	unsigned long nr_clusters = DIV_ROUND_UP(maxpages, SWAPFILE_CLUSTER);
	struct swap_cluster_info *cluster_info;
	int err = -ENOMEM;
	unsigned long i;

	cluster_info = kvcalloc(nr_clusters, sizeof(*cluster_info), GFP_KERNEL);
	if (!cluster_info)
		goto err;

	for (i = 0; i < nr_clusters; i++)
		spin_lock_init(&cluster_info[i].lock);

	if (!(si->flags & SWP_SOLIDSTATE)) {
		si->global_cluster = kmalloc(sizeof(*si->global_cluster),
				     GFP_KERNEL);
		if (!si->global_cluster)
			goto err;
		for (i = 0; i < SWAP_NR_ORDERS; i++)
			si->global_cluster->next[i] = SWAP_SLOT_INVALID;
		spin_lock_init(&si->global_cluster_lock);
	}

	/*
	 * Mark unusable pages as unavailable. The clusters aren't
	 * marked free yet, so no list operations are involved yet.
	 *
	 * See setup_swap_map(): header page, bad pages,
	 * and the EOF part of the last cluster.
	 */
	err = swap_cluster_setup_bad_slot(cluster_info, 0);
	if (err)
		goto err;
	for (i = 0; i < swap_header->info.nr_badpages; i++) {
		unsigned int page_nr = swap_header->info.badpages[i];

		if (page_nr >= maxpages)
			continue;
		err = swap_cluster_setup_bad_slot(cluster_info, page_nr);
		if (err)
			goto err;
	}
	for (i = maxpages; i < round_up(maxpages, SWAPFILE_CLUSTER); i++) {
		err = swap_cluster_setup_bad_slot(cluster_info, i);
		if (err)
			goto err;
	}

	INIT_LIST_HEAD(&si->free_clusters);
	INIT_LIST_HEAD(&si->full_clusters);
	INIT_LIST_HEAD(&si->discard_clusters);

	for (i = 0; i < SWAP_NR_ORDERS; i++) {
		INIT_LIST_HEAD(&si->nonfull_clusters[i]);
		INIT_LIST_HEAD(&si->frag_clusters[i]);
	}

	for (i = 0; i < nr_clusters; i++) {
		struct swap_cluster_info *ci = &cluster_info[i];

		if (ci->count) {
			ci->flags = CLUSTER_FLAG_NONFULL;
			list_add_tail(&ci->list, &si->nonfull_clusters[0]);
		} else {
			ci->flags = CLUSTER_FLAG_FREE;
			list_add_tail(&ci->list, &si->free_clusters);
		}
	}

	return cluster_info;
err:
	free_cluster_info(cluster_info, maxpages);
	return ERR_PTR(err);
}

SYSCALL_DEFINE2(swapon, const char __user *, specialfile, int, swap_flags)
{
	struct swap_info_struct *si;
	struct filename *name;
	struct file *swap_file = NULL;
	struct address_space *mapping;
	struct dentry *dentry;
	int prio;
	int error;
	union swap_header *swap_header;
	int nr_extents;
	sector_t span;
	unsigned long maxpages;
	unsigned long *swap_map = NULL, *bad_map = NULL;
	struct swap_cluster_info *cluster_info = NULL;
	struct folio *folio = NULL;
	struct inode *inode = NULL;
	bool inced_nr_rotate_swap = false;

	if (swap_flags & ~SWAP_FLAGS_VALID)
		return -EINVAL;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	si = alloc_swap_info();
	if (IS_ERR(si))
		return PTR_ERR(si);

	INIT_WORK(&si->discard_work, swap_discard_work);
	INIT_WORK(&si->reclaim_work, swap_reclaim_work);

	name = getname(specialfile);
	if (IS_ERR(name)) {
		error = PTR_ERR(name);
		name = NULL;
		goto bad_swap;
	}
	swap_file = file_open_name(name, O_RDWR | O_LARGEFILE | O_EXCL, 0);
	if (IS_ERR(swap_file)) {
		error = PTR_ERR(swap_file);
		swap_file = NULL;
		goto bad_swap;
	}

	si->swap_file = swap_file;
	mapping = swap_file->f_mapping;
	dentry = swap_file->f_path.dentry;
	inode = mapping->host;

	error = claim_swapfile(si, inode);
	if (unlikely(error))
		goto bad_swap;

	inode_lock(inode);
	if (d_unlinked(dentry) || cant_mount(dentry)) {
		error = -ENOENT;
		goto bad_swap_unlock_inode;
	}
	if (IS_SWAPFILE(inode)) {
		error = -EBUSY;
		goto bad_swap_unlock_inode;
	}

	/*
	 * The swap subsystem needs a major overhaul to support this.
	 * It doesn't work yet so just disable it for now.
	 */
	if (mapping_min_folio_order(mapping) > 0) {
		error = -EINVAL;
		goto bad_swap_unlock_inode;
	}

	/*
	 * Read the swap header.
	 */
	if (!mapping->a_ops->read_folio) {
		error = -EINVAL;
		goto bad_swap_unlock_inode;
	}
	folio = read_mapping_folio(mapping, 0, swap_file);
	if (IS_ERR(folio)) {
		error = PTR_ERR(folio);
		goto bad_swap_unlock_inode;
	}
	swap_header = kmap_local_folio(folio, 0);

	maxpages = read_swap_header(si, swap_header, inode);
	if (unlikely(!maxpages)) {
		error = -EINVAL;
		goto bad_swap_unlock_inode;
	}

	si->max = maxpages;
	si->pages = maxpages - 1;
	nr_extents = setup_swap_extents(si, &span);
	if (nr_extents < 0) {
		error = nr_extents;
		goto bad_swap_unlock_inode;
	}
	if (si->pages != si->max - 1) {
		pr_err("swap:%u != (max:%u - 1)\n", si->pages, si->max);
		error = -EINVAL;
		goto bad_swap_unlock_inode;
	}

	maxpages = si->max;

	/* OK, set up the swap map and apply the bad block list */
	swap_map = kvcalloc(BITS_TO_LONGS(maxpages), sizeof(long), GFP_KERNEL);
	if (!swap_map) {
		error = -ENOMEM;
		goto bad_swap_unlock_inode;
	}

	bad_map = kvcalloc(BITS_TO_LONGS(maxpages), sizeof(long), GFP_KERNEL);
	if (!bad_map) {
		error = -ENOMEM;
		goto bad_swap_unlock_inode;
	}

	error = setup_swap_map(si, swap_header, swap_map, bad_map, maxpages);
	if (error)
		goto bad_swap_unlock_inode;

	si->bad_map = bad_map;

	if (si->bdev && bdev_stable_writes(si->bdev))
		si->flags |= SWP_STABLE_WRITES;

	if (si->bdev && bdev_synchronous(si->bdev))
		si->flags |= SWP_SYNCHRONOUS_IO;

	if (si->bdev && bdev_nonrot(si->bdev)) {
		si->flags |= SWP_SOLIDSTATE;
	} else {
		atomic_inc(&nr_rotate_swap);
		inced_nr_rotate_swap = true;
	}

	cluster_info = setup_clusters(si, swap_header, maxpages);
	if (IS_ERR(cluster_info)) {
		error = PTR_ERR(cluster_info);
		cluster_info = NULL;
		goto bad_swap_unlock_inode;
	}

	if ((swap_flags & SWAP_FLAG_DISCARD) &&
	    si->bdev && bdev_max_discard_sectors(si->bdev)) {
		/*
		 * When discard is enabled for swap with no particular
		 * policy flagged, we set all swap discard flags here in
		 * order to sustain backward compatibility with older
		 * swapon(8) releases.
		 */
		si->flags |= (SWP_DISCARDABLE | SWP_AREA_DISCARD |
			     SWP_PAGE_DISCARD);

		/*
		 * By flagging sys_swapon, a sysadmin can tell us to
		 * either do single-time area discards only, or to just
		 * perform discards for released swap page-clusters.
		 * Now it's time to adjust the p->flags accordingly.
		 */
		if (swap_flags & SWAP_FLAG_DISCARD_ONCE)
			si->flags &= ~SWP_PAGE_DISCARD;
		else if (swap_flags & SWAP_FLAG_DISCARD_PAGES)
			si->flags &= ~SWP_AREA_DISCARD;

		/* issue a swapon-time discard if it's still required */
		if (si->flags & SWP_AREA_DISCARD) {
			int err = discard_swap(si);
			if (unlikely(err))
				pr_err("swapon: discard_swap(%p): %d\n",
					si, err);
		}
	}

	/*
	 * Flush any pending IO and dirty mappings before we start using this
	 * swap device.
	 */
	inode->i_flags |= S_SWAPFILE;
	error = inode_drain_writes(inode);
	if (error) {
		inode->i_flags &= ~S_SWAPFILE;
		goto bad_swap_unlock_inode;
	}

	mutex_lock(&swapon_mutex);
	prio = DEF_SWAP_PRIO;
	if (swap_flags & SWAP_FLAG_PREFER)
		prio = swap_flags & SWAP_FLAG_PRIO_MASK;
	enable_swap_info(si, prio, swap_map, cluster_info);

	pr_info("Adding %uk swap on %s.  Priority:%d extents:%d across:%lluk %s%s%s%s\n",
		K(si->pages), name->name, si->prio, nr_extents,
		K((unsigned long long)span),
		(si->flags & SWP_SOLIDSTATE) ? "SS" : "",
		(si->flags & SWP_DISCARDABLE) ? "D" : "",
		(si->flags & SWP_AREA_DISCARD) ? "s" : "",
		(si->flags & SWP_PAGE_DISCARD) ? "c" : "");

	mutex_unlock(&swapon_mutex);
	atomic_inc(&proc_poll_event);
	wake_up_interruptible(&proc_poll_wait);

	error = 0;
	goto out;
bad_swap_unlock_inode:
	inode_unlock(inode);
bad_swap:
	kfree(si->global_cluster);
	si->global_cluster = NULL;
	inode = NULL;
	destroy_swap_extents(si);
	spin_lock(&swap_lock);
	si->swap_file = NULL;
	si->flags = 0;
	spin_unlock(&swap_lock);
	if (swap_map)
		kvfree(swap_map);
	if (bad_map)
		kvfree(bad_map);
	if (cluster_info)
		free_cluster_info(cluster_info, maxpages);
	if (inced_nr_rotate_swap)
		atomic_dec(&nr_rotate_swap);
	if (swap_file)
		filp_close(swap_file, NULL);
out:
	if (!IS_ERR_OR_NULL(folio))
		folio_release_kmap(folio, swap_header);
	if (name)
		putname(name);
	if (inode)
		inode_unlock(inode);
	return error;
}

void si_swapinfo(struct sysinfo *val)
{
	unsigned int type;
	unsigned long nr_to_be_unused = 0;

	spin_lock(&swap_lock);
	for (type = 0; type < nr_swapfiles; type++) {
		struct swap_info_struct *si = swap_info[type];

		if ((si->flags & SWP_USED) && !(si->flags & SWP_WRITEOK))
			nr_to_be_unused += swap_usage_in_pages(si);
	}
	val->freeswap = atomic_long_read(&nr_swap_pages) + nr_to_be_unused;
	val->totalswap = total_swap_pages + nr_to_be_unused;
	spin_unlock(&swap_lock);
}

#if defined(CONFIG_MEMCG) && defined(CONFIG_BLK_CGROUP)
static bool __has_usable_swap(void)
{
	return !plist_head_empty(&swap_active_head);
}

void __folio_throttle_swaprate(struct folio *folio, gfp_t gfp)
{
	struct swap_info_struct *si;

	if (!(gfp & __GFP_IO))
		return;

	if (!__has_usable_swap())
		return;

	if (!blk_cgroup_congested())
		return;

	/*
	 * We've already scheduled a throttle, avoid taking the global swap
	 * lock.
	 */
	if (current->throttle_disk)
		return;

	spin_lock(&swap_avail_lock);
	plist_for_each_entry(si, &swap_avail_head, avail_list) {
		if (si->bdev) {
			blkcg_schedule_throttle(si->bdev->bd_disk, true);
			break;
		}
	}
	spin_unlock(&swap_avail_lock);
}
#endif

static int __init swapfile_init(void)
{
	swapfile_maximum_size = arch_max_swapfile_size();

	/*
	 * Once a cluster is freed, it's swap table content is read only, and
	 * all swap table readers verify the content before use. So it's safe to
	 * use RCU slab here.
	 */
	if (!SWP_TABLE_USE_PAGE)
		swap_table_cachep = kmem_cache_create("swap_table",
				    sizeof(struct swap_table),
				    0, SLAB_PANIC | SLAB_TYPESAFE_BY_RCU, NULL);

#ifdef CONFIG_MIGRATION
	if (swapfile_maximum_size >= (1UL << SWP_MIG_TOTAL_BITS))
		swap_migration_ad_supported = true;
#endif	/* CONFIG_MIGRATION */

	return 0;
}
subsys_initcall(swapfile_init);
