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
 * Helpers for accessing or modifying the swap table of a cluster,
 * the swap cluster must be locked.
 */
static inline void __swap_table_set(struct swap_cluster_info *ci,
				    unsigned int off, unsigned long swp_tb)
{
	atomic_long_t *table = rcu_dereference_protected(ci->table, true);

	lockdep_assert_held(&ci->lock);
	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	atomic_long_set(&table[off], swp_tb);
}

static inline unsigned long __swap_table_get(struct swap_cluster_info *ci,
					     unsigned int off)
{
	atomic_long_t *table;

	VM_WARN_ON_ONCE(off >= SWAPFILE_CLUSTER);
	table = rcu_dereference_check(ci->table, lockdep_is_held(&ci->lock));

	return atomic_long_read(&table[off]);
}

static inline unsigned long swap_table_get(struct swap_cluster_info *ci,
					unsigned int off)
{
	atomic_long_t *table;
	unsigned long swp_tb;

	rcu_read_lock();
	table = rcu_dereference(ci->table);
	swp_tb = table ? atomic_long_read(&table[off]) : 0;
	rcu_read_unlock();

	return swp_tb;
}
#endif
