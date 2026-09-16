/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SWAP_TIER_H
#define _SWAP_TIER_H

#include <linux/list.h>
#include <linux/plist.h>
#include <linux/types.h>

/* Forward declarations */
struct swap_info_struct;

#define TIER_ALL_MASK		(~0U)

/*
 * struct swap_tier - structure representing a swap tier.
 *
 * @prio: priority of the swap devices in the tier.
 * @nr_devs: swap devices in the tier, including ones being swapped off.
 * @active_head: swap devices in the tier.
 * @avail_head: available swap devices in the tier.
 * @list: linkage into swap_tier_active_list or swap_tier_inactive_list.
 */
struct swap_tier {
	short prio;
	int nr_devs;
	struct plist_head active_head;
	struct plist_head avail_head;
	struct list_head list;
};

extern struct list_head swap_tier_active_list;

#define for_each_active_tier(tier) \
	list_for_each_entry(tier, &swap_tier_active_list, list)

/* Initialization and application */
void swap_tiers_init(void);

/* Tier assignment */
void swap_tiers_assign_dev(struct swap_info_struct *swp);
void swap_tiers_remove_dev(struct swap_info_struct *swp);
unsigned int swap_tiers_release_dev(struct swap_info_struct *swp);
struct plist_head *swap_tiers_avail_head(struct swap_info_struct *swp);

/**
 * swap_tiers_mask_test - test whether two tier masks overlap
 * @tier_mask: mask to test, e.g. a swap device's tier bit
 * @mask: mask to test against, e.g. a cgroup's mask
 *
 * Return: true if @tier_mask and @mask share at least one tier bit.
 */
static inline bool swap_tiers_mask_test(unsigned int tier_mask,
					unsigned int mask)
{
	return tier_mask & mask;
}

#if defined(CONFIG_MEMCG) && defined(CONFIG_DEBUG_FS)
/* Memcg related functions */
void swap_tiers_memcg_propagate(unsigned int mask);
unsigned int folio_tier_mask(struct folio *folio);
#else
static inline void swap_tiers_memcg_propagate(unsigned int mask) {}

static inline unsigned int folio_tier_mask(struct folio *folio)
{
	return TIER_ALL_MASK;
}
#endif

#endif /* _SWAP_TIER_H */
