/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SWAP_TIER_H
#define _SWAP_TIER_H

#include <linux/types.h>
#include <linux/spinlock.h>

/* Forward declarations */
struct swap_info_struct;

extern spinlock_t swap_tier_lock;

/* Initialization and application */
void swap_tiers_init(void);
ssize_t swap_tiers_sysfs_show(char *buf);

int swap_tiers_add(const char *name, int prio);
int swap_tiers_remove(const char *name, int *mask);

void swap_tiers_snapshot(void);
void swap_tiers_snapshot_restore(void);
bool swap_tiers_update(int mask);

/* Tier assignment */
void swap_tiers_assign_dev(struct swap_info_struct *swp);

#define TIER_ALL_MASK		(~0)
#define TIER_DEFAULT_IDX	(31)
#define TIER_DEFAULT_MASK	(1U << TIER_DEFAULT_IDX)

#if defined(CONFIG_SWAP) && defined(CONFIG_MEMCG)
/* Memcg related functions */
void swap_tiers_mask_show(struct seq_file *m, int mask);
void swap_tiers_memcg_inherit_mask(struct mem_cgroup *memcg);
void swap_tiers_memcg_sync_mask(struct mem_cgroup *memcg);
int swap_tiers_mask_lookup(const char *name);
static inline int folio_tier_effective_mask(struct folio *folio)
{
	struct mem_cgroup *memcg;
	int mask = TIER_ALL_MASK;

	rcu_read_lock();
	memcg = folio_memcg(folio);
	if (memcg)
		mask = READ_ONCE(memcg->tier_effective_mask);
	rcu_read_unlock();

	return mask;
}
#else
static inline void swap_tiers_mask_show(struct seq_file *m, int mask) {}
static inline void swap_tiers_memcg_inherit_mask(struct mem_cgroup *memcg) {}
static inline void swap_tiers_memcg_sync_mask(struct mem_cgroup *memcg) {}
static inline int swap_tiers_mask_lookup(const char *name)
{
	return 0;
}
static inline int folio_tier_effective_mask(struct folio *folio)
{
	return TIER_ALL_MASK;
}
#endif

/**
 * swap_tiers_mask_test - Check if the tier mask is valid
 * @tier_mask: The tier mask to check
 * @mask: The mask to compare against
 *
 * Return: true if condition matches, false otherwise
 */
static inline bool swap_tiers_mask_test(int tier_mask, int mask)
{
	return tier_mask & mask;
}
#endif /* _SWAP_TIER_H */
