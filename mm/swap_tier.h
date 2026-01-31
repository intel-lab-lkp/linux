/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SWAP_TIER_H
#define _SWAP_TIER_H

#include <linux/types.h>
#include <linux/spinlock.h>

#define MAX_TIERNAME		16

/* Ensure MAX_SWAPTIER does not exceed MAX_SWAPFILES */
#if 8 > MAX_SWAPFILES
#define MAX_SWAPTIER		MAX_SWAPFILES
#else
#define MAX_SWAPTIER		8
#endif

/* Forward declarations */
struct swap_info_struct;

extern spinlock_t swap_tier_lock;

struct swap_tier_save_ctx {
	char name[MAX_TIERNAME];
	short prio;
};

#define DEFINE_SWAP_TIER_SAVE_CTX(_name) \
	struct swap_tier_save_ctx _name[MAX_SWAPTIER] = {0}

#define TIER_ALL_MASK		(~0)
#define TIER_DEFAULT_IDX	(31)
#define TIER_DEFAULT_MASK	(1 << TIER_DEFAULT_IDX)

#ifdef CONFIG_MEMCG
static inline int folio_tier_effective_mask(struct folio *folio)
{
	struct mem_cgroup *memcg = folio_memcg(folio);

	return memcg ? memcg->tier_effective_mask : TIER_ALL_MASK;
}
#else
static inline int folio_tier_effective_mask(struct folio *folio)
{
	return TIER_ALL_MASK;
}
#endif

/* Initialization and application */
void swap_tiers_init(void);
ssize_t swap_tiers_sysfs_show(char *buf);

int swap_tiers_add(const char *name, int prio);
int swap_tiers_remove(const char *name, int *mask);
int swap_tiers_modify(const char *name, int prio);

void swap_tiers_save(struct swap_tier_save_ctx ctx[]);
void swap_tiers_restore(struct swap_tier_save_ctx ctx[]);
bool swap_tiers_update(int mask);

/* Tier assignment */
void swap_tiers_assign_dev(struct swap_info_struct *swp);

/* Memcg related functions */
void swap_tiers_mask_show(struct seq_file *m, int mask);
void swap_tiers_memcg_inherit_mask(struct mem_cgroup *memcg,
	struct mem_cgroup *parent);
void swap_tiers_memcg_sync_mask(struct mem_cgroup *memcg);
void __swap_tiers_memcg_sync_mask(struct mem_cgroup *memcg);

/* Mask and tier lookup */
int swap_tiers_mask_lookup(const char *name);

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
