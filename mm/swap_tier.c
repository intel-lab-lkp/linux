// SPDX-License-Identifier: GPL-2.0
#include <linux/swap.h>

#include "swap.h"
#include "swap_tier.h"

#define MAX_SWAPTIER	MAX_SWAPFILES

static struct swap_tier swap_tiers[MAX_SWAPTIER];

/* active swap priority list, sorted in descending order */
LIST_HEAD(swap_tier_active_list);
/* unused swap_tier object */
static LIST_HEAD(swap_tier_inactive_list);

#define for_each_tier(tier, idx) \
	for (idx = 0, tier = &swap_tiers[0]; idx < MAX_SWAPTIER; \
		idx++, tier = &swap_tiers[idx])

/*
 * Naming Convention:
 *   swap_tiers_*() - Public/exported functions
 *   swap_tier_*()  - Private/internal functions
 */

static struct swap_tier *swap_tier_lookup(short prio)
{
	struct swap_tier *tier;

	for_each_active_tier(tier) {
		if (tier->prio == prio)
			return tier;
	}

	return NULL;
}

/* Insert new tier into the active list sorted by priority. */
static void swap_tier_activate(struct swap_tier *new)
{
	struct list_head *pos = &swap_tier_active_list;
	struct swap_tier *tier;

	for_each_active_tier(tier) {
		if (tier->prio <= new->prio) {
			pos = &tier->list;
			break;
		}
	}

	list_add_tail(&new->list, pos);
}

static void swap_tier_inactivate(struct swap_tier *tier)
{
	list_move_tail(&tier->list, &swap_tier_inactive_list);
}

void swap_tiers_init(void)
{
	struct swap_tier *tier;
	int idx;

	BUILD_BUG_ON(BITS_PER_TYPE(int) < MAX_SWAPTIER);

	for_each_tier(tier, idx) {
		plist_head_init(&tier->active_head);
		INIT_LIST_HEAD(&tier->list);
		swap_tier_inactivate(tier);
	}
}

static struct swap_tier *swap_tier_prepare(short prio)
{
	struct swap_tier *tier;

	lockdep_assert_held(&swap_lock);

	/* A tier holds at least one device, so one is always unused. */
	tier = list_first_entry(&swap_tier_inactive_list,
		struct swap_tier, list);

	list_del_init(&tier->list);
	tier->prio = prio;

	return tier;
}

void swap_tiers_assign_dev(struct swap_info_struct *swp)
{
	struct swap_tier *tier;

	lockdep_assert_held(&swap_lock);

	tier = swap_tier_lookup(swp->prio);
	if (!tier) {
		tier = swap_tier_prepare(swp->prio);
		swap_tier_activate(tier);
	}

	plist_add(&swp->list, &tier->active_head);
}

void swap_tiers_remove_dev(struct swap_info_struct *swp)
{
	struct swap_tier *tier;

	lockdep_assert_held(&swap_lock);

	tier = swap_tier_lookup(swp->prio);
	plist_del(&swp->list, &tier->active_head);
	if (plist_head_empty(&tier->active_head))
		swap_tier_inactivate(tier);
}
