/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_SWAP_TABLE_H
#define _MM_SWAP_TABLE_H

#include <linux/rcupdate.h>
#include <linux/atomic.h>
#include "swap.h"

/* A typical flat array in each cluster as swap table */
struct swap_table {
	atomic_long_t entries[SWAPFILE_CLUSTER];
};

#define SWP_TABLE_USE_PAGE (sizeof(struct swap_table) == PAGE_SIZE)

/*
 * The rmap table stores virtual swap entry values in each slot. The high bit
 * is reserved as a flag to indicate that the physical swap slot is in
 * "cache only" state (swap_count == 0, in_swapcache == true). This allows the
 * physical swap allocator to check this state cheaply without going through
 * the vswap layer.
 */
#define SWP_RMAP_CACHE_ONLY	(1UL << (BITS_PER_LONG - 1))
#define SWP_RMAP_ENTRY_MASK	(~SWP_RMAP_CACHE_ONLY)

/*
 * Helpers for accessing or modifying the swap table of a cluster.
 *
 * __swap_table_set uses atomic_long_set which is inherently atomic on
 * aligned longs. No lock is required for the write itself, but callers
 * must ensure exclusive ownership of the slot or hold appropriate locks
 * for their own synchronization needs.
 */
static inline void __swap_table_set(struct swap_cluster_info *ci,
				    unsigned int off, unsigned long swp_tb)
{
	atomic_long_t *table = rcu_dereference_check(ci->table, true);

	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	atomic_long_set(&table[off], swp_tb);
}

static inline unsigned long __swap_table_get(struct swap_cluster_info *ci,
					     unsigned int off)
{
	atomic_long_t *table;

	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	table = rcu_dereference_check(ci->table, lockdep_is_held(&ci->lock));

	return atomic_long_read(&table[off]) & SWP_RMAP_ENTRY_MASK;
}

static inline unsigned long swap_table_get(struct swap_cluster_info *ci,
					unsigned int off)
{
	atomic_long_t *table;
	unsigned long swp_tb;

	rcu_read_lock();
	table = rcu_dereference(ci->table);
	swp_tb = table ? atomic_long_read(&table[off]) & SWP_RMAP_ENTRY_MASK : 0;
	rcu_read_unlock();

	return swp_tb;
}

/* Mark a physical swap slot as "cache only" (swap_count == 0, in swap cache). */
static inline void swap_rmap_mark_cache_only(swp_slot_t slot)
{
	struct swap_cluster_info *ci = __swap_slot_to_cluster(slot);
	unsigned int off = swp_cluster_offset(slot);
	atomic_long_t *table;

	table = rcu_dereference_check(ci->table, true);
	atomic_long_or(SWP_RMAP_CACHE_ONLY, &table[off]);
}

/* Clear the "cache only" flag on a physical swap slot. */
static inline void swap_rmap_clear_cache_only(swp_slot_t slot)
{
	struct swap_cluster_info *ci = __swap_slot_to_cluster(slot);
	unsigned int off = swp_cluster_offset(slot);
	atomic_long_t *table;

	table = rcu_dereference_check(ci->table, true);
	atomic_long_and(~SWP_RMAP_CACHE_ONLY, &table[off]);
}

/* Check if a physical swap slot is in "cache only" state. */
static inline bool swap_rmap_is_cache_only(struct swap_cluster_info *ci,
					   unsigned int off)
{
	atomic_long_t *table;
	bool ret;

	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	rcu_read_lock();
	table = rcu_dereference(ci->table);
	ret = table && (atomic_long_read(&table[off]) & SWP_RMAP_CACHE_ONLY);
	rcu_read_unlock();
	return ret;
}
#endif
