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
 * Virtual swap slots are organized into PMD-sized clusters, analogous to
 * physical swap allocator. However, unlike the physical swap allocator,
 * the clusters are dynamically allocated and freed on-demand. There is no
 * "free list" of virtual swap clusters - new free clusters are allocated
 * directly from the cluster map xarray.
 *
 * This allows us to avoid the overhead of pre-allocating a large number of
 * virtual swap clusters.
 */

/**
 * Swap descriptor - metadata of a swapped out page.
 *
 * @slot: The handle to the physical swap slot backing this page.
 */
struct swp_desc {
	swp_slot_t slot;
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

static void __vswap_alloc_from_cluster(struct vswap_cluster *cluster, int start)
{
	int i, nr = 1 << cluster->order;
	struct swp_desc *desc;

	for (i = 0; i < nr; i++) {
		desc = &cluster->descriptors[start + i];
		desc->slot.val = 0;
	}
	cluster->count += nr;
}

static unsigned long vswap_alloc_from_cluster(struct vswap_cluster *cluster)
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
	__vswap_alloc_from_cluster(cluster, i);
	return i + (cluster->id << VSWAP_CLUSTER_SHIFT);
}

/* Allocate a contiguous range of virtual swap slots */
static swp_entry_t vswap_alloc(int order)
{
	struct xa_limit limit = vswap_cluster_map_limit;
	struct vswap_cluster *local, *cluster;
	int nr = 1 << order;
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
		entry.val = vswap_alloc_from_cluster(cluster);
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

			entry.val = vswap_alloc_from_cluster(cluster);
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
					(entry.val & VSWAP_CLUSTER_MASK));
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
static void vswap_rmap_set(struct swap_cluster_info *ci, swp_slot_t slot,
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

/**
 * vswap_free - free a virtual swap slot.
 * @entry: the virtual swap slot to free
 * @ci: the physical swap slot's cluster (optional, can be NULL)
 *
 * If @ci is NULL, this function is called to clean up a virtual swap entry
 * when no linkage has been established between physical and virtual swap slots.
 * If @ci is provided, the caller must ensure it is locked.
 */
void vswap_free(swp_entry_t entry, struct swap_cluster_info *ci)
{
	struct vswap_cluster *cluster = NULL;
	struct swp_desc *desc;

	if (!entry.val)
		return;

	swap_cache_clear_shadow(entry, 1);
	zswap_invalidate(entry);
	mem_cgroup_uncharge_swap(entry, 1);

	/* do not immediately erase the virtual slot to prevent its reuse */
	rcu_read_lock();
	desc = vswap_iter(&cluster, entry.val);
	if (!desc) {
		rcu_read_unlock();
		return;
	}

	if (desc->slot.val)
		vswap_rmap_set(ci, desc->slot, 0, 1);

	/* erase forward mapping and release the virtual slot for reallocation */
	release_vswap_slot(cluster, entry.val);
	spin_unlock(&cluster->lock);
	rcu_read_unlock();
}

/**
 * folio_alloc_swap - allocate swap space for a folio.
 * @folio: the folio.
 *
 * Return: 0, if the allocation succeeded, -ENOMEM, if the allocation failed.
 */
int folio_alloc_swap(struct folio *folio)
{
	struct vswap_cluster *cluster = NULL;
	struct swap_info_struct *si;
	struct swap_cluster_info *ci;
	int i, err, ret, nr = folio_nr_pages(folio), order = folio_order(folio);
	struct swp_desc *desc;
	swp_entry_t entry;
	swp_slot_t slot = { 0 };

	VM_BUG_ON_FOLIO(!folio_test_locked(folio), folio);
	VM_BUG_ON_FOLIO(!folio_test_uptodate(folio), folio);

	entry = vswap_alloc(folio_order(folio));
	if (!entry.val)
		return -ENOMEM;

	/*
	 * XXX: for now, we always allocate a physical swap slot for each virtual
	 * swap slot, and their lifetime are coupled. This will change once we
	 * decouple virtual swap slots from their backing states, and only allocate
	 * physical swap slots for them on demand (i.e on zswap writeback, or
	 * fallback from zswap store failure).
	 */
	ret = swap_slot_alloc(&slot, order);
	if (ret || !slot.val) {
		/* Need to call this even if allocation failed, for MEMCG_SWAP_FAIL. */
		mem_cgroup_try_charge_swap(folio, (swp_entry_t){0});

		for (i = 0; i < nr; i++)
			vswap_free((swp_entry_t){entry.val + i}, NULL);

		return ret ? ret : -ENOMEM;
	}

	/* establish the virtual <-> physical swap slots linkages. */
	si = __swap_slot_to_info(slot);
	ci = swap_cluster_lock(si, swp_slot_offset(slot));
	vswap_rmap_set(ci, slot, entry.val, nr);
	swap_cluster_unlock(ci);

	rcu_read_lock();
	for (i = 0; i < nr; i++) {
		desc = vswap_iter(&cluster, entry.val + i);
		VM_WARN_ON(!desc);

		desc->slot.val = slot.val + i;
	}
	if (cluster)
		spin_unlock(&cluster->lock);
	rcu_read_unlock();

	/*
	 * XXX: for now, we charge towards the memory cgroup's swap limit on virtual
	 * swap slots allocation. This is acceptable because as noted above, each
	 * virtual swap slot corresponds to a physical swap slot. Once we have
	 * decoupled virtual and physical swap slots, we will only charge when we
	 * actually allocate a physical swap slot.
	 */
	if (mem_cgroup_try_charge_swap(folio, entry))
		goto out_free;

	err = swap_cache_add_folio(folio, entry,
				   __GFP_HIGH | __GFP_NOMEMALLOC | __GFP_NOWARN,
				   NULL);
	if (err)
		goto out_free;

	return 0;

out_free:
	put_swap_folio(folio, entry);
	return -ENOMEM;
}

/**
 * swp_entry_to_swp_slot - look up the physical swap slot corresponding to a
 *                         virtual swap slot.
 * @entry: the virtual swap slot.
 *
 * Return: the physical swap slot corresponding to the virtual swap slot.
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

bool tryget_swap_entry(swp_entry_t entry, struct swap_info_struct **si)
{
	struct vswap_cluster *cluster;
	swp_slot_t slot;

	slot = swp_entry_to_swp_slot(entry);
	*si = swap_slot_tryget_swap_info(slot);
	if (!*si)
		return false;

	/*
	 * Ensure the cluster and its associated data structures (swap cache etc.)
	 * remain valid.
	 */
	rcu_read_lock();
	cluster = xa_load(&vswap_cluster_map, VSWAP_CLUSTER_IDX(entry));
	if (!cluster || !refcount_inc_not_zero(&cluster->refcnt)) {
		rcu_read_unlock();
		swap_slot_put_swap_info(*si);
		*si = NULL;
		return false;
	}
	rcu_read_unlock();
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
