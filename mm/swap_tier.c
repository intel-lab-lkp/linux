// SPDX-License-Identifier: GPL-2.0
#include <linux/swap.h>
#include <linux/memcontrol.h>
#include "memcontrol-v1.h"
#include <linux/sysfs.h>
#include <linux/plist.h>

#include "swap.h"
#include "swap_tier.h"

#define MAX_SWAPTIER	CONFIG_NR_SWAP_TIERS
#define MAX_TIERNAME	16

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
#define TIER_MASK(tier)	(1U << TIER_IDX(tier))
#define TIER_INACTIVE_PRIO (DEF_SWAP_PRIO - 1)
#define TIER_IS_ACTIVE(tier) ((tier->prio) !=  TIER_INACTIVE_PRIO)
#define TIER_END_PRIO(tier) \
	(!list_is_first(&(tier)->list, &swap_tier_active_list) ? \
	list_prev_entry((tier), list)->prio - 1 : SHRT_MAX)

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
	return !list_empty(&swap_tier_active_list);
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
	list_move(&tier->list, &swap_tier_inactive_list);
	tier->prio = TIER_INACTIVE_PRIO;
}

void swap_tiers_init(void)
{
	struct swap_tier *tier;
	int idx;

	BUILD_BUG_ON(BITS_PER_TYPE(int) < MAX_SWAPTIER);

	for_each_tier(tier, idx) {
		INIT_LIST_HEAD(&tier->list);
		swap_tier_inactivate(tier);
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
		len += sysfs_emit_at(buf, len, "%-16s %-5td %-11d %-11d\n",
				     tier->name,
				     TIER_IDX(tier),
				     tier->prio,
				     TIER_END_PRIO(tier));
	}
	spin_unlock(&swap_tier_lock);

	return len;
}

static struct swap_tier *swap_tier_prepare(const char *name, short prio)
{
	struct swap_tier *tier;

	lockdep_assert_held(&swap_tier_lock);

	if (prio < DEF_SWAP_PRIO)
		return ERR_PTR(-EINVAL);

	if (list_empty(&swap_tier_inactive_list))
		return ERR_PTR(-ENOSPC);

	tier = list_first_entry(&swap_tier_inactive_list,
		struct swap_tier, list);

	list_del_init(&tier->list);
	strscpy(tier->name, name, MAX_TIERNAME);
	tier->prio = prio;

	return tier;
}

static int swap_tier_check_range(short prio)
{
	struct swap_tier *tier;

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

	for_each_active_tier(tier) {
		/* No overwrite */
		if (tier->prio == prio)
			return -EINVAL;
	}

	return 0;
}

static bool swap_tier_validate_name(const char *name)
{
	int len;

	if (!name || !*name)
		return false;

	len = strlen(name);
	if (len >= MAX_TIERNAME)
		return false;

	while (*name) {
		if (!isalnum(*name) && *name != '_')
			return false;
		name++;
	}
	return true;
}

int swap_tiers_add(const char *name, int prio)
{
	int ret;
	struct swap_tier *tier;

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

	/* Duplicate check */
	if (swap_tier_lookup(name))
		return -EEXIST;

	if (!swap_tier_validate_name(name))
		return -EINVAL;

	ret = swap_tier_check_range(prio);
	if (ret)
		return ret;

	tier = swap_tier_prepare(name, prio);
	if (IS_ERR(tier)) {
		ret = PTR_ERR(tier);
		return ret;
	}

	swap_tier_activate(tier);

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

	/* Removing DEF_SWAP_PRIO merges into the higher tier. */
	if (!list_is_singular(&swap_tier_active_list)
		&& tier->prio == DEF_SWAP_PRIO)
		list_prev_entry(tier, list)->prio = DEF_SWAP_PRIO;

	swap_tier_inactivate(tier);

	return ret;
}

static struct swap_tier swap_tiers_snap[MAX_SWAPTIER];
/*
 * XXX: When multiple operations (adds and removes) are submitted in a
 * single write, reverting each individually on failure is complex and
 * error-prone. Instead, snapshot the entire state beforehand and
 * restore it wholesale if any operation fails.
 */
void swap_tiers_snapshot(void)
{
	BUILD_BUG_ON(sizeof(swap_tiers_snap) != sizeof(swap_tiers));

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

	memcpy(swap_tiers_snap, swap_tiers, sizeof(swap_tiers));
}

void swap_tiers_snapshot_restore(void)
{
	struct swap_tier *tier;
	int idx;

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

	memcpy(swap_tiers, swap_tiers_snap, sizeof(swap_tiers));

	INIT_LIST_HEAD(&swap_tier_active_list);
	INIT_LIST_HEAD(&swap_tier_inactive_list);

	/*
	 * memcpy copied snapshot-time list pointers into each tier's
	 * list_head.  Those references are stale, so re-init every
	 * tier before re-linking into the freshly initialised global
	 * lists below.
	 */
	for_each_tier(tier, idx) {
		INIT_LIST_HEAD(&tier->list);

		if (TIER_IS_ACTIVE(tier))
			swap_tier_activate(tier);
		else
			swap_tier_inactivate(tier);
	}
}

bool swap_tiers_validate(void)
{
	struct swap_tier *tier;

	/*
	 * Initial setting might not cover DEF_SWAP_PRIO.
	 * Swap tier must cover the full range (DEF_SWAP_PRIO to SHRT_MAX).
	 */
	if (swap_tier_is_active()) {
		tier = list_last_entry(&swap_tier_active_list,
			struct swap_tier, list);

		if (tier->prio != DEF_SWAP_PRIO)
			return false;
	}

	return true;
}
