// SPDX-License-Identifier: GPL-2.0
/*
 * Virtual swap space
 *
 * Copyright (C) 2024 Meta Platforms, Inc., Nhat Pham
 */
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/swap_cgroup.h>
#include <linux/cpuhotplug.h>
#include <linux/zswap.h>
#include "internal.h"
#include "swap.h"
#include "swap_table.h"

/*
 * Virtual Swap Space
 *
 * We associate with each swapped out page a virtual swap slot. This will allow
 * us to change the backing state of a swapped out page without having to
 * update every single page table entries referring to it.
 *
 * For now, there is a one-to-one correspondence between a virtual swap slot
 * and its associated physical swap slot.
 *
 * I. Allocation
 *
 * Virtual swap slots are organized into PMD-sized clusters, analogous to
 * physical swap allocator. However, unlike the physical swap allocator,
 * the clusters are dynamically allocated and freed on-demand. There is no
 * "free list" of virtual swap clusters - new free clusters are allocated
 * directly from the cluster map xarray.
 *
 * This allows us to avoid the overhead of pre-allocating a large number of
 * virtual swap clusters.
 *
 * II. Swap Entry Lifecycle
 *
 * The swap entry's lifecycle is managed at the virtual swap layer. Conceptually,
 * each virtual swap slot has a reference count, which includes:
 *
 * 1. The number of page table entries that refer to the virtual swap slot, i.e
 *    its swap count.
 *
 * 2. Whether the virtual swap slot has been added to the swap cache - if so,
 *    its reference count is incremented by 1.
 *
 * Each virtual swap slot starts out with a reference count of 1 (since it is
 * about to be added to the swap cache). Its reference count is incremented or
 * decremented every time it is mapped to or unmapped from a PTE, as well as
 * when it is added to or removed from the swap cache. Finally, when its
 * reference count reaches 0, the virtual swap slot is freed.
 *
 * Note that we do not have a reference count field per se - it is derived from
 * the swap_count and the in_swapcache fields.
 *
 * III. Backing State
 *
 * Each virtual swap slot can be backed by:
 *
 * 1. A slot on a physical swap device (i.e a swapfile or a swap partition).
 * 2. A swapped out zero-filled page.
 * 3. A compressed object in zswap.
 * 4. An in-memory folio, that is not backed by neither a physical swap device
 *    nor zswap (i.e only in swap cache). This is used for pages that are
 *    rejected by zswap, but not (yet) backed by a physical swap device,
 *    (for e.g, due to zswap.writeback = 0), or for pages that were previously
 *    stored in zswap, but has since been loaded back into memory (and has its
 *    zswap copy invalidated).
 */

/* The backing state options of a virtual swap slot */
enum swap_type {
	VSWAP_SWAPFILE,
	VSWAP_ZERO,
	VSWAP_ZSWAP,
	VSWAP_FOLIO
};

/**
 * Swap descriptor - metadata of a swapped out page.
 *
 * @slot: The handle to the physical swap slot backing this page.
 * @zswap_entry: The zswap entry associated with this swap slot.
 * @swap_cache: The folio in swap cache. If the swap entry backing type is
 *              VSWAP_FOLIO, the backend is also stored here.
 * @shadow: The shadow entry.
 * @swap_count: The number of page table entries that refer to the swap entry.
 * @memcgid: The memcg id of the owning memcg, if any.
 * @in_swapcache: Whether the swap entry is (about to be) pinned in swap cache.
 * @type: The backing store type of the swap entry.
 */
struct swp_desc {
	union {
		swp_slot_t slot;
		struct zswap_entry *zswap_entry;
	};
	union {
		struct folio *swap_cache;
		void *shadow;
	};

	unsigned int swap_count;

#ifdef CONFIG_MEMCG
	unsigned short memcgid:16;
#endif
	bool in_swapcache:1;
	enum swap_type type:2;
};

#define VSWAP_CLUSTER_SHIFT HPAGE_PMD_ORDER
#define VSWAP_CLUSTER_SIZE (1UL << VSWAP_CLUSTER_SHIFT)
#define VSWAP_CLUSTER_MASK (VSWAP_CLUSTER_SIZE - 1)

/*
 * Map from a cluster id to the number of allocated virtual swap slots in the
 * (PMD-sized) cluster. This allows us to quickly allocate an empty cluster
 * for a large folio being swapped out.
 *
 * This xarray's lock is also used as the "global" allocator lock (for e.g, to
 * synchronize global cluster lists manipulation).
 */
static DEFINE_XARRAY_FLAGS(vswap_cluster_map, XA_FLAGS_TRACK_FREE);

#if SWP_TYPE_SHIFT > 32
/*
 * In 64 bit architecture, the maximum number of virtual swap slots is capped
 * by the number of clusters (as the vswap_cluster_map xarray can only allocate
 * up to U32 clusters).
 */
#define MAX_VSWAP	\
	(((unsigned long)U32_MAX << VSWAP_CLUSTER_SHIFT) + (VSWAP_CLUSTER_SIZE - 1))
#else
/*
 * In 32 bit architecture, just make sure the range of virtual swap slots is
 * the same as the range of physical swap slots.
 */
#define MAX_VSWAP	(((MAX_SWAPFILES - 1) << SWP_TYPE_SHIFT) | SWP_OFFSET_MASK)
#endif

static const struct xa_limit vswap_cluster_map_limit = {
	.max = MAX_VSWAP >> VSWAP_CLUSTER_SHIFT,
	.min = 0,
};

static struct list_head partial_clusters_lists[SWAP_NR_ORDERS];

/**
 * struct vswap_cluster
 *
 * @lock: Spinlock protecting the cluster's data
 * @rcu: RCU head for deferred freeing when the cluster is no longer in use
 * @list: List entry for tracking in partial_clusters_lists when not fully allocated
 * @id: Unique identifier for this cluster, used to calculate swap slot values
 * @count: Number of allocated virtual swap slots in this cluster
 * @order: Order of allocation (0 for single pages, higher for contiguous ranges)
 * @cached: Whether this cluster is cached in a per-CPU variable for fast allocation
 * @full: Whether this cluster is considered full (no more allocations possible)
 * @refcnt: Reference count tracking usage of slots in this cluster
 * @bitmap: Bitmap tracking which slots in the cluster are allocated
 * @descriptors: Pointer to array of swap descriptors for each slot in the cluster
 *
 * A vswap_cluster manages a PMD-sized group of contiguous virtual swap slots.
 * It tracks which slots are allocated using a bitmap and maintains the
 * swap descriptors in an array. The cluster is reference-counted and freed when
 * all of its slots are released and the cluster is not cached. Each cluster
 * only allocates aligned slots of a single order, determined when the cluster is
 * allocated (and never change for the entire lifetime of the cluster).
 *
 * Clusters can be in the following states:
 * - Cached in per-CPU variables for fast allocation.
 * - In partial_clusters_lists when partially allocated but not cached.
 * - Marked as full when no more allocations are possible.
 */
struct vswap_cluster {
	spinlock_t lock;
	union {
		struct rcu_head rcu;
		struct list_head list;
	};
	unsigned long id;
	unsigned int count:VSWAP_CLUSTER_SHIFT + 1;
	unsigned int order:4;
	bool cached:1;
	bool full:1;
	refcount_t refcnt;
	DECLARE_BITMAP(bitmap, VSWAP_CLUSTER_SIZE);
	struct swp_desc descriptors[VSWAP_CLUSTER_SIZE];
};

#define VSWAP_VAL_CLUSTER_IDX(val) ((val) >> VSWAP_CLUSTER_SHIFT)
#define VSWAP_CLUSTER_IDX(entry) VSWAP_VAL_CLUSTER_IDX(entry.val)
#define VSWAP_IDX_WITHIN_CLUSTER_VAL(val) ((val) & VSWAP_CLUSTER_MASK)
#define VSWAP_IDX_WITHIN_CLUSTER(entry)	VSWAP_IDX_WITHIN_CLUSTER_VAL(entry.val)

struct percpu_vswap_cluster {
	struct vswap_cluster *clusters[SWAP_NR_ORDERS];
	local_lock_t lock;
};

/*
 * Per-CPU cache of the last allocated cluster for each order. This allows
 * allocation fast path to skip the global vswap_cluster_map's spinlock, if
 * the locally cached cluster still has free slots. Note that caching a cluster
 * also increments its reference count.
 */
static DEFINE_PER_CPU(struct percpu_vswap_cluster, percpu_vswap_cluster) = {
	.clusters = { NULL, },
	.lock = INIT_LOCAL_LOCK(),
};

static atomic_t vswap_alloc_reject;
static atomic_t vswap_used;

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>

static struct dentry *vswap_debugfs_root;

static int vswap_debug_fs_init(void)
{
	if (!debugfs_initialized())
		return -ENODEV;

	vswap_debugfs_root = debugfs_create_dir("vswap", NULL);
	debugfs_create_atomic_t("alloc_reject", 0444,
		vswap_debugfs_root, &vswap_alloc_reject);
	debugfs_create_atomic_t("used", 0444, vswap_debugfs_root, &vswap_used);

	return 0;
}
#else
static int vswap_debug_fs_init(void)
{
	return 0;
}
#endif

/*
 * Lockless version of vswap_iter - assumes caller holds cluster lock.
 * Used when iterating within the same cluster with the lock already held.
 */
static struct swp_desc *__vswap_iter(struct vswap_cluster *cluster, unsigned long i)
{
	unsigned long slot_index;

	lockdep_assert_held(&cluster->lock);
	VM_WARN_ON(cluster->id != VSWAP_VAL_CLUSTER_IDX(i));

	slot_index = VSWAP_IDX_WITHIN_CLUSTER_VAL(i);
	if (test_bit(slot_index, cluster->bitmap))
		return &cluster->descriptors[slot_index];

	return NULL;
}

static struct swp_desc *vswap_iter(struct vswap_cluster **clusterp, unsigned long i)
{
	unsigned long cluster_id = VSWAP_VAL_CLUSTER_IDX(i);
	struct vswap_cluster *cluster = *clusterp;
	struct swp_desc *desc = NULL;
	unsigned long slot_index;

	if (!cluster || cluster_id != cluster->id) {
		if (cluster)
			spin_unlock(&cluster->lock);
		cluster = xa_load(&vswap_cluster_map, cluster_id);
		if (!cluster)
			goto done;
		VM_WARN_ON(cluster->id != cluster_id);
		spin_lock(&cluster->lock);
	}

	slot_index = VSWAP_IDX_WITHIN_CLUSTER_VAL(i);
	if (test_bit(slot_index, cluster->bitmap))
		desc = &cluster->descriptors[slot_index];

	if (!desc) {
		spin_unlock(&cluster->lock);
		cluster = NULL;
	}

done:
	*clusterp = cluster;
	return desc;
}

static bool cluster_is_alloc_candidate(struct vswap_cluster *cluster)
{
	return cluster->count + (1 << (cluster->order)) <= VSWAP_CLUSTER_SIZE;
}

static void __vswap_alloc_from_cluster(struct vswap_cluster *cluster,
		int start, struct folio *folio)
{
	int i, nr = 1 << cluster->order;
	struct swp_desc *desc;

	for (i = 0; i < nr; i++) {
		desc = &cluster->descriptors[start + i];
		desc->type = VSWAP_FOLIO;
		desc->swap_cache = folio;
#ifdef CONFIG_MEMCG
		desc->memcgid = 0;
#endif
		desc->swap_count = 0;
		desc->in_swapcache = true;
	}
	cluster->count += nr;
}

static unsigned long vswap_alloc_from_cluster(struct vswap_cluster *cluster,
		struct folio *folio)
{
	int nr = 1 << cluster->order;
	unsigned long i = cluster->id ? 0 : nr;

	lockdep_assert_held(&cluster->lock);
	if (!cluster_is_alloc_candidate(cluster))
		return 0;

	/* Find the first free range of nr contiguous aligned slots */
	i = bitmap_find_next_zero_area(cluster->bitmap,
			VSWAP_CLUSTER_SIZE, i, nr, nr - 1);
	if (i >= VSWAP_CLUSTER_SIZE)
		return 0;

	/* Mark the range as allocated in the bitmap */
	bitmap_set(cluster->bitmap, i, nr);

	refcount_add(nr, &cluster->refcnt);
	__vswap_alloc_from_cluster(cluster, i, folio);
	return i + (cluster->id << VSWAP_CLUSTER_SHIFT);
}

/* Allocate a contiguous range of virtual swap slots */
static swp_entry_t vswap_alloc(struct folio *folio)
{
	struct xa_limit limit = vswap_cluster_map_limit;
	struct vswap_cluster *local, *cluster;
	int order = folio_order(folio), nr = 1 << order;
	bool need_caching = true;
	u32 cluster_id;
	swp_entry_t entry;

	entry.val = 0;

	/* first, let's try the locally cached cluster */
	rcu_read_lock();
	local_lock(&percpu_vswap_cluster.lock);
	cluster = this_cpu_read(percpu_vswap_cluster.clusters[order]);
	if (cluster) {
		spin_lock(&cluster->lock);
		entry.val = vswap_alloc_from_cluster(cluster, folio);
		need_caching = !entry.val;

		if (!entry.val || !cluster_is_alloc_candidate(cluster)) {
			this_cpu_write(percpu_vswap_cluster.clusters[order], NULL);
			cluster->cached = false;
			refcount_dec(&cluster->refcnt);
			cluster->full = true;
		}
		spin_unlock(&cluster->lock);
	}
	local_unlock(&percpu_vswap_cluster.lock);
	rcu_read_unlock();

	/*
	 * Local cluster does not have space. Let's try the uncached partial
	 * clusters before acquiring a new free cluster to reduce fragmentation,
	 * and avoid having to allocate a new cluster structure.
	 */
	if (!entry.val) {
		cluster = NULL;
		xa_lock(&vswap_cluster_map);
		list_for_each_entry_safe(cluster, local,
				&partial_clusters_lists[order], list) {
			if (!spin_trylock(&cluster->lock))
				continue;

			entry.val = vswap_alloc_from_cluster(cluster, folio);
			list_del_init(&cluster->list);
			cluster->full = !entry.val || !cluster_is_alloc_candidate(cluster);
			need_caching = !cluster->full;
			spin_unlock(&cluster->lock);
			if (entry.val)
				break;
		}
		xa_unlock(&vswap_cluster_map);
	}

	/* try a new free cluster */
	if (!entry.val) {
		cluster = kvzalloc(sizeof(*cluster), GFP_KERNEL);
		if (cluster) {
			/* first cluster cannot allocate a PMD-sized THP */
			if (order == SWAP_NR_ORDERS - 1)
				limit.min = 1;

			if (!xa_alloc(&vswap_cluster_map, &cluster_id, cluster, limit,
						GFP_KERNEL)) {
				spin_lock_init(&cluster->lock);
				cluster->id = cluster_id;
				cluster->order = order;
				INIT_LIST_HEAD(&cluster->list);
				/* Initialize bitmap to all zeros (all slots free) */
				bitmap_zero(cluster->bitmap, VSWAP_CLUSTER_SIZE);
				entry.val = cluster->id << VSWAP_CLUSTER_SHIFT;
				refcount_set(&cluster->refcnt, nr);
				if (!cluster_id)
					entry.val += nr;
				__vswap_alloc_from_cluster(cluster,
					(entry.val & VSWAP_CLUSTER_MASK), folio);
				/* Mark the allocated range in the bitmap */
				bitmap_set(cluster->bitmap, (entry.val & VSWAP_CLUSTER_MASK), nr);
				need_caching = cluster_is_alloc_candidate(cluster);
			} else {
				/* Failed to insert into cluster map, free the cluster */
				kvfree(cluster);
				cluster = NULL;
			}
		}
	}

	if (need_caching && entry.val) {
		local_lock(&percpu_vswap_cluster.lock);
		local = this_cpu_read(percpu_vswap_cluster.clusters[order]);
		if (local != cluster) {
			if (local) {
				spin_lock(&local->lock);
				/* only update the local cache if cached cluster is full */
				need_caching = !cluster_is_alloc_candidate(local);
				if (need_caching) {
					this_cpu_write(percpu_vswap_cluster.clusters[order], NULL);
					local->cached = false;
					refcount_dec(&local->refcnt);
				}
				spin_unlock(&local->lock);
			}

			VM_WARN_ON(!cluster);
			spin_lock(&cluster->lock);
			if (cluster_is_alloc_candidate(cluster)) {
				if (need_caching) {
					this_cpu_write(percpu_vswap_cluster.clusters[order],
					       cluster);
					refcount_inc(&cluster->refcnt);
					cluster->cached = true;
				} else {
					xa_lock(&vswap_cluster_map);
					VM_WARN_ON(!list_empty(&cluster->list));
					list_add(&cluster->list, &partial_clusters_lists[order]);
					xa_unlock(&vswap_cluster_map);
				}
			}
			spin_unlock(&cluster->lock);
		}
		local_unlock(&percpu_vswap_cluster.lock);
	}

	if (entry.val) {
		VM_WARN_ON(entry.val + nr - 1 > MAX_VSWAP);
		atomic_add(nr, &vswap_used);
	} else {
		atomic_add(nr, &vswap_alloc_reject);
	}
	return entry;
}

static void vswap_cluster_free(struct vswap_cluster *cluster)
{
	VM_WARN_ON(cluster->count || cluster->cached);
	lockdep_assert_held(&cluster->lock);
	xa_lock(&vswap_cluster_map);
	list_del_init(&cluster->list);
	__xa_erase(&vswap_cluster_map, cluster->id);
	xa_unlock(&vswap_cluster_map);
	rcu_head_init(&cluster->rcu);
	kvfree_rcu(cluster, rcu);
}

static inline void release_vswap_slot(struct vswap_cluster *cluster,
		unsigned long index)
{
	unsigned long slot_index = VSWAP_IDX_WITHIN_CLUSTER_VAL(index);

	lockdep_assert_held(&cluster->lock);
	cluster->count--;

	bitmap_clear(cluster->bitmap, slot_index, 1);

	/* we only free uncached empty clusters */
	if (refcount_dec_and_test(&cluster->refcnt))
		vswap_cluster_free(cluster);
	else if (cluster->full && cluster_is_alloc_candidate(cluster)) {
		cluster->full = false;
		if (!cluster->cached) {
			xa_lock(&vswap_cluster_map);
			VM_WARN_ON(!list_empty(&cluster->list));
			list_add_tail(&cluster->list,
				&partial_clusters_lists[cluster->order]);
			xa_unlock(&vswap_cluster_map);
		}
	}

	atomic_dec(&vswap_used);
}

/*
 * Update the physical-to-virtual swap slot mapping.
 * Caller must ensure the physical swap slot's cluster is locked.
 */
void vswap_rmap_set(struct swap_cluster_info *ci, swp_slot_t slot,
			   unsigned long vswap, int nr)
{
	atomic_long_t *table;
	unsigned long slot_offset = swp_slot_offset(slot);
	unsigned int ci_off = slot_offset % SWAPFILE_CLUSTER;
	int i;

	table = rcu_dereference_protected(ci->table, lockdep_is_held(&ci->lock));
	VM_WARN_ON(!table);
	for (i = 0; i < nr; i++)
		__swap_table_set(ci, ci_off + i, vswap ? vswap + i : 0);
}

/*
 * Caller needs to handle races with other operations themselves.
 *
 * Specifically, this function is safe to be called in contexts where the swap
 * entry has been added to the swap cache and the associated folio is locked.
 * We cannot race with other accessors, and the swap entry is guaranteed to be
 * valid the whole time (since swap cache implies one refcount).
 *
 * We cannot assume that the backends will be of the same type,
 * contiguous, etc. We might have a large folio coalesced from subpages with
 * mixed backend, which is only rectified when it is reclaimed.
 */
static void release_backing(swp_entry_t entry, int nr)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	unsigned long flush_nr, phys_swap_start = 0, phys_swap_end = 0;
	unsigned long phys_swap_released = 0;
	unsigned int phys_swap_type = 0;
	bool need_flushing_phys_swap = false;
	swp_slot_t flush_slot;
	int i;

	VM_WARN_ON(!entry.val);

	rcu_read_lock();
	for (i = 0; i < nr; i++) {
		desc = vswap_iter(&cluster, entry.val + i);
		VM_WARN_ON(!desc);

		/*
		 * We batch contiguous physical swap slots for more efficient
		 * freeing.
		 */
		if (phys_swap_start != phys_swap_end &&
				(desc->type != VSWAP_SWAPFILE ||
					swp_slot_type(desc->slot) != phys_swap_type ||
					swp_slot_offset(desc->slot) != phys_swap_end)) {
			need_flushing_phys_swap = true;
			flush_slot = swp_slot(phys_swap_type, phys_swap_start);
			flush_nr = phys_swap_end - phys_swap_start;
			phys_swap_start = phys_swap_end = 0;
		}

		if (desc->type == VSWAP_ZSWAP && desc->zswap_entry) {
			zswap_entry_free(desc->zswap_entry);
		} else if (desc->type == VSWAP_SWAPFILE) {
			phys_swap_released++;
			if (!phys_swap_start) {
				/* start a new contiguous range of phys swap */
				phys_swap_start = swp_slot_offset(desc->slot);
				phys_swap_end = phys_swap_start + 1;
				phys_swap_type = swp_slot_type(desc->slot);
			} else {
				/* extend the current contiguous range of phys swap */
				phys_swap_end++;
			}
		}

		desc->slot.val = 0;

		if (need_flushing_phys_swap) {
			spin_unlock(&cluster->lock);
			cluster = NULL;
			swap_slot_free_nr(flush_slot, flush_nr);
			need_flushing_phys_swap = false;
		}
	}
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();

	/* Flush any remaining physical swap range */
	if (phys_swap_start) {
		flush_slot = swp_slot(phys_swap_type, phys_swap_start);
		flush_nr = phys_swap_end - phys_swap_start;
		swap_slot_free_nr(flush_slot, flush_nr);
	}

	if (phys_swap_released)
		mem_cgroup_uncharge_swap(entry, phys_swap_released);
}

/*
 * Entered with the cluster locked, but might unlock the cluster.
 * This is because several operations, such as releasing physical swap slots
 * (i.e swap_slot_free_nr()) require the cluster to be unlocked to avoid
 * deadlocks.
 *
 * This is safe, because:
 *
 * 1. The swap entry to be freed has refcnt (swap count and swapcache pin)
 *    down to 0, so no one can change its internal state
 *
 * 2. The swap entry to be freed still holds a refcnt to the cluster, keeping
 *    the cluster itself valid.
 *
 * We will exit the function with the cluster re-locked.
 */
static void vswap_free(struct vswap_cluster *cluster, struct swp_desc *desc,
	swp_entry_t entry)
{
	/* Clear shadow if present */
	if (xa_is_value(desc->shadow))
		desc->shadow = NULL;
	spin_unlock(&cluster->lock);

	release_backing(entry, 1);
	mem_cgroup_clear_swap(entry, 1);

	/* erase forward mapping and release the virtual slot for reallocation */
	spin_lock(&cluster->lock);
	release_vswap_slot(cluster, entry.val);
}

/**
 * folio_alloc_swap - allocate virtual swap space for a folio.
 * @folio: the folio.
 *
 * Return: 0, if the allocation succeeded, -ENOMEM, if the allocation failed.
 */
int folio_alloc_swap(struct folio *folio)
{
	swp_entry_t entry;

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_uptodate(folio), folio);

	entry = vswap_alloc(folio);
	if (!entry.val)
		return -ENOMEM;

	mem_cgroup_record_swap(folio, entry);
	swap_cache_add_folio(folio, entry, NULL);

	return 0;
}

/**
 * vswap_alloc_swap_slot - allocate physical swap space for a folio that is
 *                         already associated with virtual swap slots.
 * @folio: folio we want to allocate physical swap space for.
 *
 * Note that this does NOT release existing swap backends of the folio.
 * Callers need to handle this themselves.

 * Return: true if the folio is now backed by physical swap slots, false
 * otherwise.
 */
bool vswap_alloc_swap_slot(struct folio *folio)
{
	int i, nr = folio_nr_pages(folio);
	struct vswap_cluster *cluster = NULL;
	struct swap_info_struct *si;
	struct swap_cluster_info *ci;
	swp_slot_t slot = { .val = 0 };
	swp_entry_t entry = folio->swap;
	struct swp_desc *desc;
	bool fallback = false;

	/*
	 * We might have already allocated a backing physical swap slot in past
	 * attempts (for instance, when we disable zswap). If the entire range is
	 * already swapfile-backed we can skip swapfile case.
	 */
	if (vswap_swapfile_backed(entry, nr))
		return true;

	if (swap_slot_alloc(&slot, folio_order(folio)))
		return false;

	if (!slot.val)
		return false;

	if (mem_cgroup_try_charge_swap(folio, entry)) {
		/*
		 * We have not updated the backing type of the virtual swap slot.
		 * Simply free up the physical swap slots here!
		 */
		swap_slot_free_nr(slot, nr);
		return false;
	}

	/* establish the vrtual <-> physical swap slots linkages. */
	si = __swap_slot_to_info(slot);
	ci = swap_cluster_lock(si, swp_slot_offset(slot));
	vswap_rmap_set(ci, slot, entry.val, nr);
	swap_cluster_unlock(ci);

	rcu_read_lock();
	for (i = 0; i < nr; i++) {
		desc = vswap_iter(&cluster, entry.val + i);
		VM_WARN_ON(!desc);

		if (desc->type == VSWAP_FOLIO) {
			/* case 1: fallback from zswap store failure */
			fallback = true;
			VM_WARN_ON(folio != desc->swap_cache);
		} else {
			/*
			 * Case 2: zswap writeback.
			 *
			 * No need to free zswap entry here - it will be freed once zswap
			 * writeback suceeds.
			 */
			VM_WARN_ON(desc->type != VSWAP_ZSWAP);
			VM_WARN_ON(fallback);
		}
		desc->type = VSWAP_SWAPFILE;
		desc->slot.val = slot.val + i;
	}
	spin_unlock(&cluster->lock);
	rcu_read_unlock();
	return true;
}

/**
 * swp_entry_to_swp_slot - look up the physical swap slot corresponding to a
 *                         virtual swap slot.
 * @entry: the virtual swap slot.
 *
 * Return: the physical swap slot corresponding to the virtual swap slot, if
 * exists, or the zero physical swap slot if the virtual swap slot is not
 * backed by any physical slot on a swapfile.
 */
swp_slot_t swp_entry_to_swp_slot(swp_entry_t entry)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	swp_slot_t slot;

	slot.val = 0;
	if (!entry.val)
		return slot;

	rcu_read_lock();
	desc = vswap_iter(&cluster, entry.val);
	if (!desc) {
		rcu_read_unlock();
		return (swp_slot_t){0};
	}

	if (desc->type != VSWAP_SWAPFILE)
		slot.val = 0;
	else
		slot = desc->slot;
	spin_unlock(&cluster->lock);
	rcu_read_unlock();

	return slot;
}

/**
 * swp_slot_to_swp_entry - look up the virtual swap slot corresponding to a
 *                         physical swap slot.
 * @slot: the physical swap slot.
 *
 * Return: the virtual swap slot corresponding to the physical swap slot.
 */
swp_entry_t swp_slot_to_swp_entry(swp_slot_t slot)
{
	swp_entry_t ret;
	struct swap_cluster_info *ci;
	unsigned long offset;
	unsigned int ci_off;

	ret.val = 0;
	if (!slot.val)
		return ret;

	offset = swp_slot_offset(slot);
	ci_off = offset % SWAPFILE_CLUSTER;
	ci = __swap_slot_to_cluster(slot);

	ret.val = swap_table_get(ci, ci_off);
	return ret;
}

/*
 * Decrease the swap count of nr contiguous swap entries by 1 (when the swap
 * entries are removed from a range of PTEs), and check if any of the swap
 * entries are in swap cache only after its swap count is decreased.
 *
 * The check is racy, but it is OK because free_swap_and_cache_nr() only use
 * the result as a hint.
 */
static bool vswap_free_nr_any_cache_only(swp_entry_t entry, int nr)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	bool ret = false;
	int i;

	rcu_read_lock();
	for (i = 0; i < nr; i++) {
		desc = vswap_iter(&cluster, entry.val);
		VM_WARN_ON(!desc);
		ret |= (desc->swap_count == 1 && desc->in_swapcache);
		desc->swap_count--;
		if (!desc->swap_count && !desc->in_swapcache)
			vswap_free(cluster, desc, entry);
		entry.val++;
	}
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();
	return ret;
}

/**
 * swap_free_nr - decrease the swap count of nr contiguous swap entries by 1
 *                (when the swap entries are removed from a range of PTEs).
 * @entry: the first entry in the range.
 * @nr: the number of entries in the range.
 */
void swap_free_nr(swp_entry_t entry, int nr)
{
	vswap_free_nr_any_cache_only(entry, nr);
}

static int swap_duplicate_nr(swp_entry_t entry, int nr)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	int i = 0;

	rcu_read_lock();
	for (i = 0; i < nr; i++) {
		desc = vswap_iter(&cluster, entry.val + i);
		if (!desc || (!desc->swap_count && !desc->in_swapcache))
			goto done;
		desc->swap_count++;
	}
done:
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();
	if (i && i < nr)
		swap_free_nr(entry, i);

	return i == nr ? 0 : -ENOENT;
}

/**
 * swap_duplicate - increase the swap count of the swap entry by 1 (i.e when
 *                  the swap entry is stored at a new PTE).
 * @entry: the swap entry.
 *
 * Return: -ENONENT, if we try to duplicate a non-existent swap entry.
 */
int swap_duplicate(swp_entry_t entry)
{
	return swap_duplicate_nr(entry, 1);
}


bool folio_swapped(struct folio *folio)
{
	struct vswap_cluster *cluster = NULL;
	swp_entry_t entry = folio->swap;
	int i, nr = folio_nr_pages(folio);
	struct swp_desc *desc;
	bool swapped = false;

	if (!entry.val)
		return false;

	rcu_read_lock();
	for (i = 0; i < nr; i++) {
		desc = vswap_iter(&cluster, entry.val + i);
		if (desc && desc->swap_count) {
			swapped = true;
			break;
		}
	}
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();
	return swapped;
}

/**
 * swp_swapcount - return the swap count of the swap entry.
 * @id: the swap entry.
 *
 * Note that all the swap count functions are identical in the new design,
 * since we no longer need swap count continuation.
 *
 * Return: the swap count of the swap entry.
 */
int swp_swapcount(swp_entry_t entry)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	unsigned int ret;

	rcu_read_lock();
	desc = vswap_iter(&cluster, entry.val);
	ret = desc ? desc->swap_count : 0;
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();

	return ret;
}

int __swap_count(swp_entry_t entry)
{
	return swp_swapcount(entry);
}

bool swap_entry_swapped(swp_entry_t entry)
{
	return !!swp_swapcount(entry);
}

void swap_shmem_alloc(swp_entry_t entry, int nr)
{
	swap_duplicate_nr(entry, nr);
}

void swapcache_clear(swp_entry_t entry, int nr)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	int i;

	if (!nr)
		return;

	rcu_read_lock();
	for (i = 0; i < nr; i++) {
		desc = vswap_iter(&cluster, entry.val);
		desc->in_swapcache = false;
		if (!desc->swap_count)
			vswap_free(cluster, desc, entry);
		entry.val++;
	}
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();
}

int swapcache_prepare(swp_entry_t entry, int nr)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	int i, ret = 0;

	rcu_read_lock();
	for (i = 0; i < nr; i++) {
		desc = vswap_iter(&cluster, entry.val + i);

		if (!desc) {
			ret = -ENOENT;
			goto done;
		}

		if (!desc->swap_count && !desc->in_swapcache) {
			ret = -ENOENT;
			goto done;
		}

		if (desc->in_swapcache) {
			ret = -EEXIST;
			goto done;
		}

		desc->in_swapcache = true;
	}
done:
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();
	if (i && i < nr)
		swapcache_clear(entry, i);
	if (i < nr && !ret)
		ret = -ENOENT;
	return ret;
}

/**
 * is_swap_cached - check if the swap entry is cached
 * @entry: swap entry to check
 *
 * Returns true if the swap entry is cached, false otherwise.
 */
bool is_swap_cached(swp_entry_t entry)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	bool cached;

	rcu_read_lock();
	desc = vswap_iter(&cluster, entry.val);
	cached = desc ? desc->in_swapcache : false;
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();

	return cached;
}

/**
 * vswap_only_has_cache - check if all the slots in the range are still valid,
 *                        and are in swap cache only (i.e not stored in any
 *                        PTEs).
 * @entry: the first slot in the range.
 * @nr: the number of slots in the range.
 *
 * Return: true if all the slots in the range are still valid, and are in swap
 * cache only, or false otherwise.
 */
bool vswap_only_has_cache(swp_entry_t entry, int nr)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	int i = 0;

	rcu_read_lock();
	for (i = 0; i < nr; i++) {
		desc = vswap_iter(&cluster, entry.val + i);
		if (!desc || desc->swap_count || !desc->in_swapcache)
			goto done;
	}
done:
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();
	return i == nr;
}

/**
 * non_swapcache_batch - count the longest range starting from a particular
 *                       swap slot that are stil valid, but not in swap cache.
 * @entry: the first slot to check.
 * @max_nr: the maximum number of slots to check.
 *
 * Return: the number of slots in the longest range that are still valid, but
 * not in swap cache.
 */
int non_swapcache_batch(swp_entry_t entry, int max_nr)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	int i;

	if (!entry.val)
		return 0;

	rcu_read_lock();
	for (i = 0; i < max_nr; i++) {
		desc = vswap_iter(&cluster, entry.val + i);
		if (!desc || desc->in_swapcache || !desc->swap_count)
			goto done;
	}
done:
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();
	return i;
}

/**
 * vswap_store_folio - set a folio as the backing of a range of virtual swap
 *                     slots.
 * @entry: the first virtual swap slot in the range.
 * @folio: the folio.
 */
void vswap_store_folio(swp_entry_t entry, struct folio *folio)
{
	struct vswap_cluster *cluster = NULL;
	int i, nr = folio_nr_pages(folio);
	struct swp_desc *desc;

	VM_BUG_ON(!folio_test_locked(folio));
	VM_BUG_ON(folio->swap.val != entry.val);

	release_backing(entry, nr);

	rcu_read_lock();
	for (i = 0; i < nr; i++) {
		desc = vswap_iter(&cluster, entry.val + i);
		VM_WARN_ON(!desc);
		desc->type = VSWAP_FOLIO;
		desc->swap_cache = folio;
	}
	spin_unlock(&cluster->lock);
	rcu_read_unlock();
}

/**
 * swap_zeromap_folio_set - mark a range of virtual swap slots corresponding to
 *                          a folio as zero-filled.
 * @folio: the folio
 */
void swap_zeromap_folio_set(struct folio *folio)
{
	struct obj_cgroup *objcg = get_obj_cgroup_from_folio(folio);
	struct vswap_cluster *cluster = NULL;
	swp_entry_t entry = folio->swap;
	int i, nr = folio_nr_pages(folio);
	struct swp_desc *desc;

	VM_BUG_ON(!folio_test_locked(folio));
	VM_BUG_ON(!entry.val);

	release_backing(entry, nr);

	rcu_read_lock();
	for (i = 0; i < nr; i++) {
		desc = vswap_iter(&cluster, entry.val + i);
		VM_WARN_ON(!desc);
		desc->type = VSWAP_ZERO;
	}
	spin_unlock(&cluster->lock);
	rcu_read_unlock();

	count_vm_events(SWPOUT_ZERO, nr);
	if (objcg) {
		count_objcg_events(objcg, SWPOUT_ZERO, nr);
		obj_cgroup_put(objcg);
	}
}

/*
 * Iterate through the entire range of virtual swap slots, returning the
 * longest contiguous range of slots starting from the first slot that satisfies:
 *
 * 1. If the first slot is zero-mapped, the entire range should be
 *    zero-mapped.
 * 2. If the first slot is backed by a swapfile, the entire range should
 *    be backed by a range of contiguous swap slots on the same swapfile.
 * 3. If the first slot is zswap-backed, the entire range should be
 *    zswap-backed.
 * 4. If the first slot is backed by a folio, the entire range should
 *    be backed by the same folio.
 *
 * Note that this check is racy unless we can ensure that the entire range
 * has their backing state stable - for instance, if the caller was the one
 * who set the swap cache pin.
 */
static int vswap_check_backing(swp_entry_t entry, enum swap_type *type, int nr)
{
	unsigned int swapfile_type;
	struct vswap_cluster *cluster = NULL;
	enum swap_type first_type;
	struct swp_desc *desc;
	pgoff_t first_offset;
	struct folio *folio;
	int i = 0;

	if (!entry.val)
		return 0;

	rcu_read_lock();
	for (i = 0; i < nr; i++) {
		desc = vswap_iter(&cluster, entry.val + i);
		if (!desc)
			goto done;

		if (!i) {
			first_type = desc->type;
			if (first_type == VSWAP_SWAPFILE) {
				swapfile_type = swp_slot_type(desc->slot);
				first_offset = swp_slot_offset(desc->slot);
			} else if (first_type == VSWAP_FOLIO) {
				folio = desc->swap_cache;
			}
		} else if (desc->type != first_type) {
			goto done;
		} else if (first_type == VSWAP_SWAPFILE &&
				(swp_slot_type(desc->slot) != swapfile_type ||
					swp_slot_offset(desc->slot) != first_offset + i)) {
			goto done;
		} else if (first_type == VSWAP_FOLIO && desc->swap_cache != folio) {
			goto done;
		}
	}
done:
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();
	if (type)
		*type = first_type;
	return i;
}

/**
 * vswap_swapfile_backed - check if the virtual swap slots are backed by physical
 *                         swap slots.
 * @entry: the first entry in the range.
 * @nr: the number of entries in the range.
 */
bool vswap_swapfile_backed(swp_entry_t entry, int nr)
{
	enum swap_type type;

	return vswap_check_backing(entry, &type, nr) == nr
				&& type == VSWAP_SWAPFILE;
}

/**
 * vswap_folio_backed - check if the virtual swap slots are backed by in-memory
 *                      pages.
 * @entry: the first virtual swap slot in the range.
 * @nr: the number of slots in the range.
 */
bool vswap_folio_backed(swp_entry_t entry, int nr)
{
	enum swap_type type;

	return vswap_check_backing(entry, &type, nr) == nr && type == VSWAP_FOLIO;
}

/**
 * vswap_can_swapin_thp - check if the swap entries can be swapped in as a THP.
 * @entry: the first virtual swap slot in the range.
 * @nr: the number of slots in the range.
 *
 * For now, we can only swap in a THP if the entire range is zero-filled, or if
 * the entire range is backed by a contiguous range of physical swap slots on a
 * swapfile.
 */
bool vswap_can_swapin_thp(swp_entry_t entry, int nr)
{
	enum swap_type type;

	return vswap_check_backing(entry, &type, nr) == nr &&
		(type == VSWAP_ZERO || type == VSWAP_SWAPFILE);
}

/*
 * Return the count of contiguous swap entries that share the same
 * VSWAP_ZERO status as the starting entry. If is_zeromap is not NULL,
 * it will return the VSWAP_ZERO status of the starting entry.
 */
int swap_zeromap_batch(swp_entry_t entry, int max_nr, bool *is_zeromap)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	int i = 0;
	bool is_zero = false;

	VM_WARN_ON(!entry.val);

	rcu_read_lock();
	for (i = 0; i < max_nr; i++) {
		desc = vswap_iter(&cluster, entry.val + i);
		if (!desc)
			goto done;

		if (!i)
			is_zero = (desc->type == VSWAP_ZERO);
		else if ((desc->type == VSWAP_ZERO) != is_zero)
			goto done;
	}
done:
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();
	if (i && is_zeromap)
		*is_zeromap = is_zero;

	return i;
}

/**
 * free_swap_and_cache_nr() - Release a swap count on range of swap entries and
 *                            reclaim their cache if no more references remain.
 * @entry: First entry of range.
 * @nr: Number of entries in range.
 *
 * For each swap entry in the contiguous range, release a swap count. If any
 * swap entries have their swap count decremented to zero, try to reclaim their
 * associated swap cache pages.
 */
void free_swap_and_cache_nr(swp_entry_t entry, int nr)
{
	int i = 0, incr = 1;
	struct folio *folio;

	if (vswap_free_nr_any_cache_only(entry, nr)) {
		while (i < nr) {
			incr = 1;
			if (vswap_only_has_cache(entry, 1)) {
				folio = swap_cache_get_folio(entry);
				if (!folio)
					goto next;

				if (!folio_trylock(folio)) {
					folio_put(folio);
					goto next;
				}

				if (!folio_matches_swap_entry(folio, entry)) {
					folio_unlock(folio);
					folio_put(folio);
					goto next;
				}

				/*
				 * Folios are always naturally aligned in swap so
				 * advance forward to the next boundary.
				 */
				incr = ALIGN(entry.val + 1, folio_nr_pages(folio)) - entry.val;
				folio_free_swap(folio);
				folio_unlock(folio);
				folio_put(folio);
			}
next:
			i += incr;
			entry.val += incr;
		}
	}
}

/*
 * Called after dropping swapcache to decrease refcnt to swap entries.
 */
void put_swap_folio(struct folio *folio, swp_entry_t entry)
{
	int nr = folio_nr_pages(folio);

	VM_WARN_ON(!folio_test_locked(folio));
	swapcache_clear(entry, nr);
}

bool tryget_swap_entry(swp_entry_t entry, struct swap_info_struct **si)
{
	struct vswap_cluster *cluster;
	swp_slot_t slot;

	/*
	 * Ensure the cluster and its associated data structures (swap cache etc.)
	 * remain valid.
	 */
	rcu_read_lock();
	cluster = xa_load(&vswap_cluster_map, VSWAP_CLUSTER_IDX(entry));
	if (!cluster || !refcount_inc_not_zero(&cluster->refcnt)) {
		rcu_read_unlock();
		*si = NULL;
		return false;
	}
	rcu_read_unlock();

	slot = swp_entry_to_swp_slot(entry);
	/*
	 * Note that this function does not provide any guarantee that the virtual
	 * swap slot's backing state will be stable. This has several implications:
	 *
	 * 1. We have to obtain a reference to the swap device itself, because we
	 * need swap device's metadata in certain scenarios, for example when we
	 * need to inspect the swap device flag in do_swap_page().
	 *
	 * 2. The swap device we are looking up here might be outdated by the time we
	 * return to the caller. It is perfectly OK, if the swap_info_struct is only
	 * used in a best-effort manner (i.e optimization). If we need the precise
	 * backing state, we need to re-check after the entry is pinned in swapcache.
	 */
	if (slot.val)
		*si = swap_slot_tryget_swap_info(slot);
	else
		*si = NULL;

	return true;
}

void put_swap_entry(swp_entry_t entry, struct swap_info_struct *si)
{
	struct vswap_cluster *cluster;

	if (si)
		swap_slot_put_swap_info(si);

	rcu_read_lock();
	cluster = xa_load(&vswap_cluster_map, VSWAP_CLUSTER_IDX(entry));
	spin_lock(&cluster->lock);
	if (refcount_dec_and_test(&cluster->refcnt))
		vswap_cluster_free(cluster);
	spin_unlock(&cluster->lock);
	rcu_read_unlock();
}

/*
 * Check if two virtual swap entries belong to the same vswap cluster.
 * Useful for optimizing readahead when entries in the same cluster
 * share protection from a pinned target entry.
 */
bool vswap_same_cluster(swp_entry_t entry1, swp_entry_t entry2)
{
	return VSWAP_CLUSTER_IDX(entry1) == VSWAP_CLUSTER_IDX(entry2);
}

static int vswap_cpu_dead(unsigned int cpu)
{
	struct vswap_cluster *cluster;
	int order;

	guard(rcu)();
	for (order = 0; order < SWAP_NR_ORDERS; order++) {
		cluster = per_cpu(percpu_vswap_cluster.clusters[order], cpu);
		if (cluster) {
			per_cpu(percpu_vswap_cluster.clusters[order], cpu) = NULL;
			spin_lock(&cluster->lock);
			cluster->cached = false;
			if (refcount_dec_and_test(&cluster->refcnt))
				vswap_cluster_free(cluster);
			spin_unlock(&cluster->lock);
		}
	}

	return 0;
}

/**
 * swap_cache_lock - lock the swap cache for a swap entry
 * @entry: the swap entry
 *
 * Locks the vswap cluster spinlock for the given swap entry.
 */
void swap_cache_lock(swp_entry_t entry)
{
	struct vswap_cluster *cluster;
	unsigned long cluster_id = VSWAP_CLUSTER_IDX(entry);

	rcu_read_lock();
	cluster = xa_load(&vswap_cluster_map, cluster_id);
	VM_WARN_ON(!cluster);
	spin_lock(&cluster->lock);
	rcu_read_unlock();
}

/**
 * swap_cache_unlock - unlock the swap cache for a swap entry
 * @entry: the swap entry
 *
 * Unlocks the vswap cluster spinlock for the given swap entry.
 */
void swap_cache_unlock(swp_entry_t entry)
{
	struct vswap_cluster *cluster;
	unsigned long cluster_id = VSWAP_CLUSTER_IDX(entry);

	rcu_read_lock();
	cluster = xa_load(&vswap_cluster_map, cluster_id);
	VM_WARN_ON(!cluster);
	spin_unlock(&cluster->lock);
	rcu_read_unlock();
}

/**
 * swap_cache_lock_irq - lock the swap cache with interrupts disabled
 * @entry: the swap entry
 *
 * Locks the vswap cluster spinlock and disables interrupts for the given swap entry.
 */
void swap_cache_lock_irq(swp_entry_t entry)
{
	struct vswap_cluster *cluster;
	unsigned long cluster_id = VSWAP_CLUSTER_IDX(entry);

	rcu_read_lock();
	cluster = xa_load(&vswap_cluster_map, cluster_id);
	VM_WARN_ON(!cluster);
	spin_lock_irq(&cluster->lock);
	rcu_read_unlock();
}

/**
 * swap_cache_unlock_irq - unlock the swap cache with interrupts enabled
 * @entry: the swap entry
 *
 * Unlocks the vswap cluster spinlock and enables interrupts for the given swap entry.
 */
void swap_cache_unlock_irq(swp_entry_t entry)
{
	struct vswap_cluster *cluster;
	unsigned long cluster_id = VSWAP_CLUSTER_IDX(entry);

	rcu_read_lock();
	cluster = xa_load(&vswap_cluster_map, cluster_id);
	VM_WARN_ON(!cluster);
	spin_unlock_irq(&cluster->lock);
	rcu_read_unlock();
}

/**
 * swap_cache_get_folio - Looks up a folio in the swap cache.
 * @entry: swap entry used for the lookup.
 *
 * A found folio will be returned unlocked and with its refcount increased.
 *
 * Context: Caller must ensure @entry is valid and protect the cluster with
 * reference count or locks.
 *
 * Return: Returns the found folio on success, NULL otherwise. The caller
 * must lock and check if the folio still matches the swap entry before
 * use (e.g., folio_matches_swap_entry).
 */
struct folio *swap_cache_get_folio(swp_entry_t entry)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	struct folio *folio;

	for (;;) {
		rcu_read_lock();
		desc = vswap_iter(&cluster, entry.val);
		if (!desc) {
			rcu_read_unlock();
			return NULL;
		}

		/* Check if this is a shadow value (xa_is_value equivalent) */
		if (xa_is_value(desc->shadow)) {
			spin_unlock(&cluster->lock);
			rcu_read_unlock();
			return NULL;
		}

		folio = desc->swap_cache;
		if (!folio) {
			spin_unlock(&cluster->lock);
			rcu_read_unlock();
			return NULL;
		}

		if (likely(folio_try_get(folio))) {
			spin_unlock(&cluster->lock);
			rcu_read_unlock();
			return folio;
		}
		spin_unlock(&cluster->lock);
		rcu_read_unlock();
	}

	return NULL;
}

/**
 * swap_cache_get_shadow - Looks up a shadow in the swap cache.
 * @entry: swap entry used for the lookup.
 *
 * Context: Caller must ensure @entry is valid and protect the cluster with
 * reference count or locks.
 *
 * Return: Returns either NULL or an XA_VALUE (shadow).
 */
void *swap_cache_get_shadow(swp_entry_t entry)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	void *shadow;

	rcu_read_lock();
	desc = vswap_iter(&cluster, entry.val);
	if (!desc) {
		rcu_read_unlock();
		return NULL;
	}

	shadow = desc->shadow;
	spin_unlock(&cluster->lock);
	rcu_read_unlock();

	if (xa_is_value(shadow))
		return shadow;
	return NULL;
}

/**
 * swap_cache_add_folio - Add a folio into the swap cache.
 * @folio: The folio to be added.
 * @entry: The swap entry corresponding to the folio.
 * @shadowp: If a shadow is found, return the shadow.
 *
 * Context: Caller must ensure @entry is valid and protect the cluster with
 * reference count or locks.
 *
 * The caller also needs to obtain the swap entries' swap cache pins to avoid
 * race or conflict.
 */
void swap_cache_add_folio(struct folio *folio, swp_entry_t entry, void **shadowp)
{
	struct vswap_cluster *cluster;
	unsigned long nr_pages = folio_nr_pages(folio);
	unsigned long cluster_id = VSWAP_CLUSTER_IDX(entry);
	unsigned long i;
	struct swp_desc *desc;
	void *old;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(folio_test_swapcache(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapbacked(folio), folio);

	folio_ref_add(folio, nr_pages);
	folio_set_swapcache(folio);
	folio->swap = entry;

	rcu_read_lock();
	cluster = xa_load(&vswap_cluster_map, cluster_id);
	VM_WARN_ON(!cluster);
	spin_lock_irq(&cluster->lock);

	for (i = 0; i < nr_pages; i++) {
		desc = __vswap_iter(cluster, entry.val + i);
		VM_WARN_ON(!desc);
		old = desc->shadow;

		/* Warn if slot is already occupied by a folio */
		VM_WARN_ON_FOLIO(old && !xa_is_value(old) && old != folio, folio);

		/* Save shadow if found and not yet saved */
		if (shadowp && xa_is_value(old) && !*shadowp)
			*shadowp = old;

		desc->swap_cache = folio;
	}

	spin_unlock_irq(&cluster->lock);
	rcu_read_unlock();

	node_stat_mod_folio(folio, NR_FILE_PAGES, nr_pages);
	lruvec_stat_mod_folio(folio, NR_SWAPCACHE, nr_pages);
}

/**
 * __swap_cache_del_folio - Removes a folio from the swap cache.
 * @folio: The folio.
 * @entry: The first swap entry that the folio corresponds to.
 * @shadow: shadow value to be filled in the swap cache.
 *
 * Removes a folio from the swap cache and fills a shadow in place.
 * This won't put the folio's refcount. The caller has to do that.
 *
 * Context: Caller must ensure the folio is locked and in the swap cache
 * using the index of @entry, and lock the swap cache.
 */
void __swap_cache_del_folio(struct folio *folio, swp_entry_t entry, void *shadow)
{
	long nr_pages = folio_nr_pages(folio);
	struct vswap_cluster *cluster;
	struct swp_desc *desc;
	unsigned long cluster_id = VSWAP_CLUSTER_IDX(entry);
	int i;

	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(!folio_test_swapcache(folio), folio);
	VM_WARN_ON_ONCE_FOLIO(folio_test_writeback(folio), folio);

	rcu_read_lock();
	cluster = xa_load(&vswap_cluster_map, cluster_id);
	VM_WARN_ON(!cluster);

	for (i = 0; i < nr_pages; i++) {
		desc = __vswap_iter(cluster, entry.val + i);
		VM_WARN_ON_FOLIO(!desc || desc->swap_cache != folio, folio);
		desc->shadow = shadow;
	}
	rcu_read_unlock();

	folio->swap.val = 0;
	folio_clear_swapcache(folio);
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
	swp_entry_t entry = folio->swap;

	swap_cache_lock_irq(entry);
	__swap_cache_del_folio(folio, entry, NULL);
	swap_cache_unlock_irq(entry);

	put_swap_folio(folio, entry);
	folio_ref_sub(folio, folio_nr_pages(folio));
}

/**
 * __swap_cache_replace_folio - Replace a folio in the swap cache.
 * @old: The old folio to be replaced.
 * @new: The new folio.
 *
 * Replace an existing folio in the swap cache with a new folio. The
 * caller is responsible for setting up the new folio's flag and swap
 * entries. Replacement will take the new folio's swap entry value as
 * the starting offset to override all slots covered by the new folio.
 *
 * Context: Caller must ensure both folios are locked, and lock the
 * swap cache.
 */
void __swap_cache_replace_folio(struct folio *old, struct folio *new)
{
	swp_entry_t entry = new->swap;
	unsigned long nr_pages = folio_nr_pages(new);
	struct vswap_cluster *cluster;
	struct swp_desc *desc;
	unsigned long cluster_id = VSWAP_CLUSTER_IDX(entry);
	void *old_entry;
	int i;

	VM_WARN_ON_ONCE(!folio_test_swapcache(old) || !folio_test_swapcache(new));
	VM_WARN_ON_ONCE(!folio_test_locked(old) || !folio_test_locked(new));
	VM_WARN_ON_ONCE(!entry.val);

	rcu_read_lock();
	cluster = xa_load(&vswap_cluster_map, cluster_id);
	VM_WARN_ON(!cluster);

	for (i = 0; i < nr_pages; i++) {
		desc = __vswap_iter(cluster, entry.val + i);
		VM_WARN_ON(!desc);
		old_entry = desc->swap_cache;
		VM_WARN_ON(!old_entry || xa_is_value(old_entry) || old_entry != old);
		desc->swap_cache = new;
	}
	rcu_read_unlock();
}

#ifdef CONFIG_ZSWAP
/**
 * zswap_entry_store - store a zswap entry for a swap entry
 * @swpentry: the swap entry
 * @entry: the zswap entry to store
 *
 * Stores a zswap entry in the swap descriptor for the given swap entry.
 * Releases the old backend if one existed.
 */
void zswap_entry_store(swp_entry_t swpentry, struct zswap_entry *entry)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;

	release_backing(swpentry, 1);

	rcu_read_lock();
	desc = vswap_iter(&cluster, swpentry.val);
	VM_WARN_ON(!desc);
	desc->zswap_entry = entry;
	desc->type = VSWAP_ZSWAP;
	spin_unlock(&cluster->lock);
	rcu_read_unlock();
}

/**
 * zswap_entry_load - load a zswap entry for a swap entry
 * @swpentry: the swap entry
 *
 * Loads the zswap entry from the swap descriptor for the given swap entry.
 *
 * Return: the zswap entry if one exists, NULL otherwise
 */
void *zswap_entry_load(swp_entry_t swpentry)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	enum swap_type type;
	void *zswap_entry;

	rcu_read_lock();
	desc = vswap_iter(&cluster, swpentry.val);
	if (!desc) {
		rcu_read_unlock();
		return NULL;
	}

	type = desc->type;
	zswap_entry = desc->zswap_entry;
	spin_unlock(&cluster->lock);
	rcu_read_unlock();

	if (type != VSWAP_ZSWAP)
		return NULL;

	return zswap_entry;
}

bool zswap_empty(swp_entry_t swpentry)
{
	return xa_empty(&vswap_cluster_map);
}
#endif /* CONFIG_ZSWAP */

#ifdef CONFIG_MEMCG
/*
 * __vswap_cgroup_record - record mem_cgroup for a set of swap entries.
 *
 * Entered with the cluster locked. We will exit the function with the cluster
 * still locked.
 */
static unsigned short __vswap_cgroup_record(struct vswap_cluster *cluster,
				swp_entry_t entry, unsigned short memcgid,
				unsigned int nr_ents)
{
	struct swp_desc *desc;
	unsigned short oldid, iter = 0;
	int i;

	for (i = 0; i < nr_ents; i++) {
		desc = __vswap_iter(cluster, entry.val + i);
		VM_WARN_ON(!desc);
		oldid = desc->memcgid;
		desc->memcgid = memcgid;
		if (!iter)
			iter = oldid;
		VM_WARN_ON(iter != oldid);
	}

	return oldid;
}

static unsigned short vswap_cgroup_record(swp_entry_t entry,
				unsigned short memcgid, unsigned int nr_ents)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	unsigned short oldid;

	rcu_read_lock();
	desc = vswap_iter(&cluster, entry.val);
	VM_WARN_ON(!desc);
	oldid = __vswap_cgroup_record(cluster, entry, memcgid, nr_ents);
	spin_unlock(&cluster->lock);
	rcu_read_unlock();

	return oldid;
}

/**
 * swap_cgroup_record - record mem_cgroup for a set of swap entries.
 * These entries must belong to one single folio, and that folio
 * must be being charged for swap space (swap out), and these
 * entries must not have been charged
 *
 * @folio: the folio that the swap entry belongs to
 * @memcgid: mem_cgroup ID to be recorded
 * @entry: the first swap entry to be recorded
 */
void swap_cgroup_record(struct folio *folio, unsigned short memcgid,
			swp_entry_t entry)
{
	unsigned short oldid =
		vswap_cgroup_record(entry, memcgid, folio_nr_pages(folio));

	VM_WARN_ON(oldid);
}

/**
 * __swap_cgroup_record - record mem_cgroup for a set of swap entries.
 *
 * Same as swap_cgroup_record, but assumes the swap cache (vswap cluster)
 * lock is already held.
 *
 * @folio: the folio that the swap entry belongs to
 * @memcgid: mem_cgroup ID to be recorded
 * @entry: the first swap entry to be recorded
 */
void __swap_cgroup_record(struct folio *folio, unsigned short memcgid,
			  swp_entry_t entry)
{
	struct vswap_cluster *cluster;
	unsigned long cluster_id = VSWAP_CLUSTER_IDX(entry);
	unsigned short oldid;

	rcu_read_lock();
	cluster = xa_load(&vswap_cluster_map, cluster_id);
	VM_WARN_ON(!cluster);
	oldid = __vswap_cgroup_record(cluster, entry, memcgid,
				      folio_nr_pages(folio));
	rcu_read_unlock();

	VM_WARN_ON(oldid);
}

/**
 * swap_cgroup_clear - clear mem_cgroup for a set of swap entries.
 * These entries must be being uncharged from swap. They either
 * belongs to one single folio in the swap cache (swap in for
 * cgroup v1), or no longer have any users (slot freeing).
 *
 * @entry: the first swap entry to be recorded into
 * @nr_ents: number of swap entries to be recorded
 *
 * Returns the existing old value.
 */
unsigned short swap_cgroup_clear(swp_entry_t entry, unsigned int nr_ents)
{
	return vswap_cgroup_record(entry, 0, nr_ents);
}

/**
 * lookup_swap_cgroup_id - lookup mem_cgroup id tied to swap entry
 * @entry: swap entry to be looked up.
 *
 * Returns ID of mem_cgroup at success. 0 at failure. (0 is invalid ID)
 */
unsigned short lookup_swap_cgroup_id(swp_entry_t entry)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;
	unsigned short ret;

	/*
	 * Note that the virtual swap slot can be freed under us, for instance in
	 * the invocation of mem_cgroup_swapin_charge_folio. We need to wrap the
	 * entire lookup in RCU read-side critical section, and double check the
	 * existence of the swap descriptor.
	 */
	rcu_read_lock();
	desc = vswap_iter(&cluster, entry.val);
	ret = desc ? desc->memcgid : 0;
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();
	return ret;
}
#endif /* CONFIG_MEMCG */

int vswap_init(void)
{
	int i;

	if (cpuhp_setup_state_nocalls(CPUHP_MM_VSWAP_DEAD, "mm/vswap:dead", NULL,
				vswap_cpu_dead)) {
		pr_err("Failed to register vswap CPU hotplug callback\n");
		return -ENOMEM;
	}

	if (vswap_debug_fs_init())
		pr_warn("Failed to initialize vswap debugfs\n");

	for (i = 0; i < SWAP_NR_ORDERS; i++)
		INIT_LIST_HEAD(&partial_clusters_lists[i]);

	return 0;
}

void vswap_exit(void)
{
}
