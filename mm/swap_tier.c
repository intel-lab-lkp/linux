// SPDX-License-Identifier: GPL-2.0
#include <linux/swap.h>
#include <linux/memcontrol.h>
#include "memcontrol-v1.h"
#include <linux/sysfs.h>
#include <linux/plist.h>

#include "swap.h"
#include "swap_tier.h"

/*
 * struct swap_tier - structure representing a swap tier.
 *
 * @name: name of the swap_tier.
 * @prio: starting value of priority.
 * @list: linked list of tiers.
 */
static struct swap_tier {
	char name[MAX_TIERNAME];
	short prio;
	struct list_head list;
} swap_tiers[MAX_SWAPTIER];

DEFINE_SPINLOCK(swap_tier_lock);
/* active swap priority list, sorted in descending order */
static LIST_HEAD(swap_tier_active_list);
/* unused swap_tier object */
static LIST_HEAD(swap_tier_inactive_list);

#define TIER_IDX(tier)	((tier) - swap_tiers)
#define TIER_MASK(tier)	(1 << TIER_IDX(tier))
#define TIER_INVALID_PRIO (DEF_SWAP_PRIO - 1)
#define TIER_END_PRIO(tier) \
	(!list_is_first(&(tier)->list, &swap_tier_active_list) ? \
	list_prev_entry((tier), list)->prio - 1 : SHRT_MAX)

#define MASK_TO_TIER(mask) (&swap_tiers[__ffs((mask))])

#define for_each_tier(tier, idx) \
	for (idx = 0, tier = &swap_tiers[0]; idx < MAX_SWAPTIER; \
		idx++, tier = &swap_tiers[idx])

#define for_each_active_tier(tier) \
	list_for_each_entry(tier, &swap_tier_active_list, list)

#define for_each_inactive_tier(tier) \
	list_for_each_entry(tier, &swap_tier_inactive_list, list)

/*
 * Naming Convention:
 *   swap_tiers_*() - Public/exported functions
 *   swap_tier_*()  - Private/internal functions
 */

static bool swap_tier_is_active(void)
{
	return !list_empty(&swap_tier_active_list) ? true : false;
}

static bool swap_tier_prio_in_range(struct swap_tier *tier, short prio)
{
	if (tier->prio <= prio && TIER_END_PRIO(tier) >= prio)
		return true;

	return false;
}

static bool swap_tier_prio_is_used(struct swap_tier *self, short prio)
{
	struct swap_tier *tier;

	for_each_active_tier(tier) {
		if (tier != self && tier->prio == prio)
			return true;
	}

	return false;
}

static struct swap_tier *swap_tier_lookup(const char *name)
{
	struct swap_tier *tier;

	for_each_active_tier(tier) {
		if (!strcmp(tier->name, name))
			return tier;
	}

	return NULL;
}


void swap_tiers_init(void)
{
	struct swap_tier *tier;
	int idx;

	BUILD_BUG_ON(BITS_PER_TYPE(int) < MAX_SWAPTIER);
	BUILD_BUG_ON(MAX_SWAPTIER > TIER_DEFAULT_IDX);

	for_each_tier(tier, idx) {
		INIT_LIST_HEAD(&tier->list);
		list_add_tail(&tier->list, &swap_tier_inactive_list);
	}
}

ssize_t swap_tiers_sysfs_show(char *buf)
{
	struct swap_tier *tier;
	ssize_t len = 0;

	len += sysfs_emit_at(buf, len, "%-16s %-5s %-11s %-11s\n",
			 "Name", "Idx", "PrioStart", "PrioEnd");

	spin_lock(&swap_tier_lock);
	for_each_active_tier(tier) {
		len += sysfs_emit_at(buf, len, "%-16s %-5ld %-11d %-11d\n",
				     tier->name,
				     TIER_IDX(tier),
				     tier->prio,
				     TIER_END_PRIO(tier));
		if (len >= PAGE_SIZE)
			break;
	}
	spin_unlock(&swap_tier_lock);

	return len;
}

static void swap_tier_insert_by_prio(struct swap_tier *new)
{
	struct swap_tier *tier;

	for_each_active_tier(tier) {
		if (tier->prio > new->prio)
			continue;

		list_add_tail(&new->list, &tier->list);
		return;
	}
	/* First addition, or becomes the first tier */
	list_add_tail(&new->list, &swap_tier_active_list);
}

static void __swap_tier_prepare(struct swap_tier *tier, const char *name,
	short prio)
{
	list_del_init(&tier->list);
	strscpy(tier->name, name, MAX_TIERNAME);
	tier->prio = prio;
}

static struct swap_tier *swap_tier_prepare(const char *name, short prio)
{
	struct swap_tier *tier;

	lockdep_assert_held(&swap_tier_lock);

	if (prio < DEF_SWAP_PRIO)
		return ERR_PTR(-EINVAL);

	if (list_empty(&swap_tier_inactive_list))
		return ERR_PTR(-EPERM);

	tier = list_first_entry(&swap_tier_inactive_list,
		struct swap_tier, list);

	__swap_tier_prepare(tier, name, prio);
	return tier;
}

static int swap_tier_can_split_range(struct swap_tier *orig_tier,
	short new_prio)
{
	struct swap_info_struct *p;
	struct swap_tier *tier;

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

	plist_for_each_entry(p, &swap_active_head, list) {
		if (p->tier_mask == TIER_DEFAULT_MASK)
			continue;

		tier = MASK_TO_TIER(p->tier_mask);
		if (tier->prio > new_prio)
			continue;
		/*
                 * Prohibit implicit tier reassignment.
		 * Case 1: Prevent orig_tier devices from dropping out
		 *         of the new range.
		 */
		if (orig_tier == tier && (p->prio < new_prio))
			return -EBUSY;
                /*
                 * Case 2: Prevent other tier devices from entering
                 *         the new range.
                 */
		else if (orig_tier != tier && (p->prio >= new_prio))
			return -EBUSY;
	}

	return 0;
}

int swap_tiers_add(const char *name, int prio)
{
	int ret;
	struct swap_tier *tier;

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

	/* Duplicate check */
	if (swap_tier_lookup(name))
		return -EPERM;

	if (swap_tier_prio_is_used(NULL, prio))
		return -EBUSY;

	ret = swap_tier_can_split_range(NULL, prio);
	if (ret)
		return ret;

	tier = swap_tier_prepare(name, prio);
	if (IS_ERR(tier)) {
		ret = PTR_ERR(tier);
		return ret;
	}

	swap_tier_insert_by_prio(tier);
	return ret;
}

int swap_tiers_remove(const char *name)
{
	int ret = 0;
	struct swap_tier *tier;

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

	tier = swap_tier_lookup(name);
	if (!tier)
		return -EINVAL;

	/* Simulate adding a tier to check for conflicts */
	ret = swap_tier_can_split_range(NULL, tier->prio);
	if (ret)
		return ret;

	/* Removing DEF_SWAP_PRIO merges into the higher tier. */
	if (!list_is_singular(&swap_tier_active_list)
		&& tier->prio == DEF_SWAP_PRIO)
		list_prev_entry(tier, list)->prio = DEF_SWAP_PRIO;

	list_move(&tier->list, &swap_tier_inactive_list);
	return ret;
}

int swap_tiers_modify(const char *name, int prio)
{
	int ret;
	struct swap_tier *tier;

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

	tier = swap_tier_lookup(name);
	if (!tier)
		return -EINVAL;

	/* No need to modify */
	if (tier->prio == prio)
		return 0;

	if (swap_tier_prio_is_used(tier, prio))
		return -EBUSY;

	ret = swap_tier_can_split_range(tier, prio);
	if (ret)
		return ret;

	list_del_init(&tier->list);
	tier->prio = prio;
	swap_tier_insert_by_prio(tier);

	return ret;
}

/*
 * XXX: Reverting individual operations becomes complex as the number of
 * operations grows. Instead, we save the original state beforehand and
 * fully restore it if any operation fails.
 */
void swap_tiers_save(struct swap_tier_save_ctx ctx[])
{
	struct swap_tier *tier;
	int idx;

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

	for_each_active_tier(tier) {
		idx = TIER_IDX(tier);
		strcpy(ctx[idx].name, tier->name);
		ctx[idx].prio = tier->prio;
	}

	for_each_inactive_tier(tier) {
		idx = TIER_IDX(tier);
		/* Indicator of inactive */
		ctx[idx].prio = TIER_INVALID_PRIO;
	}
}

void swap_tiers_restore(struct swap_tier_save_ctx ctx[])
{
	struct swap_tier *tier;
	int idx;

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

	/* Invalidate active list */
	list_splice_tail_init(&swap_tier_active_list,
			&swap_tier_inactive_list);

	for_each_tier(tier, idx) {
		if (ctx[idx].prio != TIER_INVALID_PRIO) {
			/* Preserve idx(mask) */
			__swap_tier_prepare(tier, ctx[idx].name, ctx[idx].prio);
			swap_tier_insert_by_prio(tier);
		}
	}
}

void swap_tiers_assign_dev(struct swap_info_struct *swp)
{
	struct swap_tier *tier;

	lockdep_assert_held(&swap_lock);

	for_each_active_tier(tier) {
		if (swap_tier_prio_in_range(tier, swp->prio)) {
			swp->tier_mask = TIER_MASK(tier);
			return;
		}
	}

	swp->tier_mask = TIER_DEFAULT_MASK;
}

bool swap_tiers_update(void)
{
	struct swap_tier *tier;
	struct swap_info_struct *swp;

	/*
	 * Initial setting might not cover DEF_SWAP_PRIO.
	 * Swap tier must cover the full range (DEF_SWAP_PRIO to SHRT_MAX).
	 * Also, modify operation can change only one remaining priority.
	 */
	if (swap_tier_is_active()) {
		tier = list_last_entry(&swap_tier_active_list,
			struct swap_tier, list);

		if (tier->prio != DEF_SWAP_PRIO)
			return false;
	}

	/*
	 * If applied initially, the swap tier_mask may change
	 * from the default value.
	 */
	plist_for_each_entry(swp, &swap_active_head, list) {
		/* Tier is already configured */
		if (swp->tier_mask != TIER_DEFAULT_MASK)
			break;
		swap_tiers_assign_dev(swp);
	}

	return true;
}
