/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SWAP_TIER_H
#define _SWAP_TIER_H

#include <linux/list.h>
#include <linux/plist.h>
#include <linux/types.h>

/* Forward declarations */
struct swap_info_struct;

/*
 * struct swap_tier - structure representing a swap tier.
 *
 * @prio: priority of the swap devices in the tier.
 * @active_head: swap devices in the tier.
 * @avail_head: available swap devices in the tier.
 * @list: linkage into swap_tier_active_list or swap_tier_inactive_list.
 */
struct swap_tier {
	short prio;
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
struct plist_head *swap_tiers_avail_head(struct swap_info_struct *swp);

#endif /* _SWAP_TIER_H */
