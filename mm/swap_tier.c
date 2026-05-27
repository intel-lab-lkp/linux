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
	return !list_empty(&swap_tier_active_list);
}

static bool swap_tier_prio_in_range(struct swap_tier *tier, short prio)
{
	if (tier->prio <= prio && TIER_END_PRIO(tier) >= prio)
		return true;

	return false;
}

static bool swap_tier_prio_is_used(short prio)
{
	struct swap_tier *tier;

	for_each_active_tier(tier) {
		if (tier->prio == prio)
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
	BUILD_BUG_ON(MAX_SWAPTIER > TIER_DEFAULT_IDX);

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

static int swap_tier_can_split_range(short new_prio)
{
	struct swap_info_struct *p;
	struct swap_tier *tier;

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

	plist_for_each_entry(p, &swap_active_head, list) {
		if (p->tier_mask == TIER_DEFAULT_MASK)
			continue;

		tier = MASK_TO_TIER(p->tier_mask);
		if (!swap_tier_prio_in_range(tier, new_prio))
			continue;

		/*
		 * Device sits in a tier that spans new_prio;
		 * splitting here would reassign it to a
		 * different tier.
		 */
		if (p->prio >= new_prio)
			return -EBUSY;
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

	/* No overwrite */
	if (swap_tier_prio_is_used(prio))
		return -EBUSY;

	ret = swap_tier_can_split_range(prio);
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

int swap_tiers_remove(const char *name, int *mask)
{
	int ret = 0;
	struct swap_tier *tier;

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

	tier = swap_tier_lookup(name);
	if (!tier)
		return -EINVAL;

	/* Simulate adding a tier to check for conflicts */
	ret = swap_tier_can_split_range(tier->prio);
	if (ret)
		return ret;

	/* Removing DEF_SWAP_PRIO merges into the higher tier. */
	if (!list_is_singular(&swap_tier_active_list)
		&& tier->prio == DEF_SWAP_PRIO)
		list_prev_entry(tier, list)->prio = DEF_SWAP_PRIO;

	swap_tier_inactivate(tier);
	*mask |= TIER_MASK(tier);

	return ret;
}

/*
 * XXX: Static global snapshot buffer for batch operations. Small
 * and used once per write, so a static global is not bad.
 * When multiple adds/removes are submitted in a single write,
 * reverting each individually on failure is error-prone. Instead,
 * snapshot beforehand and restore wholesale if any operation fails.
 */
static struct swap_tier swap_tiers_snap[MAX_SWAPTIER];

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

#ifdef CONFIG_MEMCG
static void swap_tier_memcg_propagate(int mask)
{
	struct mem_cgroup *child;

	rcu_read_lock();
	for_each_mem_cgroup_tree(child, root_mem_cgroup) {
		WRITE_ONCE(child->tier_mask, child->tier_mask | mask);
		WRITE_ONCE(child->tier_effective_mask,
			   child->tier_effective_mask | mask);
	}
	rcu_read_unlock();
}
#else
static void swap_tier_memcg_propagate(int mask)
{
}
#endif

bool swap_tiers_update(int mask)
{
	struct swap_tier *tier;
	struct swap_info_struct *swp;

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

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

	/*
	 * When a tier is removed, its index (bit position in the mask) becomes
	 * free for reassignment to a future tier. If a memcg had previously
	 * disabled this tier (cleared the bit in its swap.tiers file), the
	 * effective mask would keep that bit clear -- meaning the new tier at
	 * the same index would be silently unavailable, an invisible cgroup
	 * constraint left behind by a tier that no longer exists.
	 *
	 * To prevent this, OR the removed tier's mask bit into every memcg's
	 * tier_mask and tier_effective_mask. This resets the bit so the new
	 * tier is accessible by default; users who want to restrict it must
	 * explicitly disable it after the tier is re-created.
	 */
	if (mask)
		swap_tier_memcg_propagate(mask);

	return true;
}

#ifdef CONFIG_MEMCG
void swap_tiers_mask_show(struct seq_file *m, int mask)
{
	struct swap_tier *tier;

	spin_lock(&swap_tier_lock);
	for_each_active_tier(tier) {
		if (mask & TIER_MASK(tier))
			seq_printf(m, "%s ", tier->name);
	}
	spin_unlock(&swap_tier_lock);
	seq_puts(m, "\n");
}

int swap_tiers_mask_lookup(const char *name)
{
	struct swap_tier *tier;

	lockdep_assert_held(&swap_tier_lock);

	for_each_active_tier(tier) {
		if (!strcmp(name, tier->name))
			return TIER_MASK(tier);
	}

	return 0;
}

static void __swap_tier_memcg_inherit_mask(struct mem_cgroup *memcg,
	struct mem_cgroup *parent)
{
	int parent_mask = parent
		? READ_ONCE(parent->tier_effective_mask)
		: TIER_ALL_MASK;

	WRITE_ONCE(memcg->tier_effective_mask,
		   parent_mask & READ_ONCE(memcg->tier_mask));
}

/* Computes the initial effective mask from the parent's effective mask. */
void swap_tiers_memcg_inherit_mask(struct mem_cgroup *memcg)
{
	spin_lock(&swap_tier_lock);
	rcu_read_lock();
	memcg->tier_mask = TIER_ALL_MASK;
	__swap_tier_memcg_inherit_mask(memcg, parent_mem_cgroup(memcg));
	rcu_read_unlock();
	spin_unlock(&swap_tier_lock);
}

/*
 * Called when a memcg's tier_mask is modified. Walks the subtree
 * and recomputes each descendant's effective mask against its parent.
 */
void swap_tiers_memcg_sync_mask(struct mem_cgroup *memcg)
{
	struct mem_cgroup *child;

	lockdep_assert_held(&swap_tier_lock);

	rcu_read_lock();
	for_each_mem_cgroup_tree(child, memcg)
		__swap_tier_memcg_inherit_mask(child, parent_mem_cgroup(child));
	rcu_read_unlock();
}
#endif
