// SPDX-License-Identifier: GPL-2.0
#include <linux/swap.h>
#include <linux/rcupdate.h>
#include <linux/memcontrol.h>
#include <linux/plist.h>
#include <linux/sysfs.h>
#include <linux/sort.h>

#include "swap_tier.h"

/*
 * struct swap_tier - Structure representing a swap tier.
 *
 * @name:		Name of the swap_tier.
 * @prio_st:		Starting value of priority.
 * @prio_end:		Ending value of priority.
 * @list:		Linked list of active tiers.
 * @inactive_list:	Linked list of inactive tiers.
 * @kref:		Reference count (held by swap device and memory cgroup).
 */
static struct swap_tier {
	char name[MAX_TIERNAME];
	short prio_st;
	short prio_end;
	union {
		struct plist_node list;
		struct list_head inactive_list;
	};
	struct kref ref;
} swap_tiers[MAX_SWAPTIER];

static DEFINE_SPINLOCK(swap_tier_lock);

/* Active swap priority list, reserved tier first, sorted in descending order */
static PLIST_HEAD(swap_tier_active_list);
/* Unused swap_tier object */
static LIST_HEAD(swap_tier_inactive_list);

#define TIER_IDX(tier)	((tier) - swap_tiers)

#define for_each_active_tier(tier) \
	plist_for_each_entry(tier, &swap_tier_active_list, list)

#define for_each_tier(tier, idx) \
	for (idx = 0, tier = &swap_tiers[0]; idx < MAX_SWAPTIER; \
		idx++, tier = &swap_tiers[idx])

static int nr_swaptiers = SWAP_TIER_RESERVED_CNT;

#define SKIP_REPLACE_IDX	-1
#define NULL_TIER               -1

/*
 * Naming Convention:
 *   swap_tiers_*() - Public/exported functions
 *   swap_tier_*()  - Private/internal functions
 */

static bool swap_tier_replace_marked(int idx)
{
	return idx != SKIP_REPLACE_IDX;
}

static bool swap_tier_is_active(void)
{
	return nr_swaptiers > SWAP_TIER_RESERVED_CNT;
}

static bool swap_tier_prio_in_range(struct swap_tier *tier, short prio)
{
	if (tier->prio_st <= prio && tier->prio_end >= prio)
		return true;

	return false;
}

/* swap_tiers initialization */
void swap_tiers_init(void)
{
	struct swap_tier *tier;
	int idx;

	BUILD_BUG_ON(BITS_PER_TYPE(int) < MAX_SWAPTIER * 2);

	for_each_tier(tier, idx) {
		if (idx < SWAP_TIER_RESERVED_CNT) {
			/* for display fisrt */
			plist_node_init(&tier->list, -SHRT_MAX);
			plist_add(&tier->list, &swap_tier_active_list);
			kref_init(&tier->ref);
		} else {
			INIT_LIST_HEAD(&tier->inactive_list);
			list_add_tail(&tier->inactive_list, &swap_tier_inactive_list);
		}
	}

	strscpy(swap_tiers[SWAP_TIER_DEFAULT].name, DEFAULT_TIER_NAME);
#ifdef CONFIG_ZSWAP
	strscpy(swap_tiers[SWAP_TIER_ZSWAP].name, ZSWAP_TIER_NAME);
#endif
}

static bool swap_tier_alive(struct swap_tier *tier)
{
	lockdep_assert_held(&swap_tier_lock);
	return (kref_read(&tier->ref) > 1) ? true : false;
}

static void swap_tier_get(struct swap_tier *tier)
{
	lockdep_assert_held(&swap_tier_lock);
	kref_get(&tier->ref);
}

static void swap_tier_remove(struct swap_tier *tier);
static void swap_tier_release(struct kref *ref)
{
	struct swap_tier *tier = container_of(ref, struct swap_tier, ref);

	swap_tier_remove(tier);
}

static void swap_tier_put(struct swap_tier *tier)
{
	lockdep_assert_held(&swap_tier_lock);
	kref_put(&tier->ref, swap_tier_release);
}

static int swap_tier_cmp(const void *p, const void *p2)
{
	const struct tiers_desc *tp = p;
	const struct tiers_desc *tp2 = p2;

	return tp2->prio_st - tp->prio_st;
}

static void swap_tier_get_mask(int mask)
{
	struct swap_tier *tier;

	lockdep_assert_held(&swap_tier_lock);

	for_each_active_tier(tier) {
		int tier_mask = TIER_MASK(TIER_IDX(tier), TIER_FULL_MASK);

		if (mask & tier_mask)
			swap_tier_get(tier);
	}
}

static void swap_tier_put_mask(int mask)
{
	struct swap_tier *tier;

	lockdep_assert_held(&swap_tier_lock);

	for_each_active_tier(tier) {
		int tier_mask = TIER_MASK(TIER_IDX(tier), TIER_FULL_MASK);

		if (mask & tier_mask)
			swap_tier_put(tier);
	}
}

static bool swap_tier_is_default(struct swap_tier *tier)
{
	int idx = TIER_IDX(tier);

	return idx < SWAP_TIER_RESERVED_CNT ? true : false;
}

/*
 * Check if tier priority range can be split. If input falls within a
 * referenced range, splitting is not allowed and an error is returned.
 * If priority start is the same but tier has no reference, mark
 * SKIP_REPLACE_IDX to allow replacement at apply time.
 */
static int swap_tier_can_split_range(struct tiers_desc *desc)
{
	struct swap_tier *tier;
	int ret = 0;

	lockdep_assert_held(&swap_tier_lock);
	desc->tier_idx = SKIP_REPLACE_IDX;

	for_each_active_tier(tier) {
		if (swap_tier_is_default(tier))
			continue;

		if (tier->prio_st > desc->prio_st)
			continue;

		/* If start priorities match, replace tier name */
		if (tier->prio_st == desc->prio_st)
			desc->tier_idx = TIER_IDX(tier);

		if (swap_tier_alive(tier))
			ret = -EBUSY;

		break;
	}

	return ret;
}

static void swap_tier_prepare(struct tiers_desc *desc)
{
	struct swap_tier *tier;

	lockdep_assert_held(&swap_tier_lock);

	if (WARN_ON(list_empty(&swap_tier_inactive_list)))
		return;

	if (swap_tier_replace_marked(desc->tier_idx))
		return;

	tier = list_first_entry(&swap_tier_inactive_list,
		struct swap_tier, inactive_list);

	list_del(&tier->inactive_list);
	strscpy(tier->name, desc->name, MAX_TIERNAME);
	tier->prio_st = desc->prio_st;
	plist_node_init(&tier->list, -tier->prio_st);
	kref_init(&tier->ref);

	plist_add(&tier->list, &swap_tier_active_list);
	nr_swaptiers++;
}

static void swap_tier_apply(struct tiers_desc *desc, int nr)
{
	struct plist_node *pnode;
	struct swap_info_struct *p = NULL;
	struct swap_tier *tier;
	int prio_end = SHRT_MAX;

	lockdep_assert_held(&swap_lock);
	lockdep_assert_held(&swap_tier_lock);

	for (int i = 0; i < nr; i++) {
		int idx = desc[i].tier_idx;

		if (swap_tier_replace_marked(idx))
			strscpy(swap_tiers[idx].name, desc[i].name, MAX_TIERNAME);
	}

	for_each_active_tier(tier) {
		if (swap_tier_is_default(tier))
			continue;

		tier->prio_end = prio_end;
		prio_end = tier->prio_st - 1;
	}

	/* TODO: use swapfiles ?*/
	if (plist_head_empty(&swap_active_head))
		return;

	for_each_active_tier(tier) {
		plist_for_each_continue(pnode, &swap_active_head) {
			p = container_of(pnode, struct swap_info_struct, list);
			if (p->tier_idx != NULL_TIER)
				continue;
			if (!swap_tier_prio_in_range(tier, p->prio))
				break;
			swap_tier_get(tier);
		}
	}
}

int swap_tiers_add(struct tiers_desc desc[], int nr)
{
	struct swap_tier *tier;
	int ret = 0;

	if (nr <= 0)
		return -EINVAL;

	sort(desc, nr, sizeof(desc[0]), swap_tier_cmp, NULL);

	for (int i = 0; i < nr; i++) {
		if (i > 0 && desc[i].prio_st == desc[i-1].prio_st)
			return -EINVAL;

		for (int j = i+1; j < nr; j++) {
			if (!strcmp(desc[i].name, desc[j].name))
				return -EINVAL;
		}

		for_each_active_tier(tier) {
			if (!strcmp(desc[i].name, tier->name))
				return -EALREADY;
		}
	}

	spin_lock(&swap_lock);
	spin_lock(&swap_tier_lock);

	/* Tier set must cover all priorities */
	if (!swap_tier_is_active() && desc[nr-1].prio_st != DEF_SWAP_PRIO)
		desc[nr-1].prio_st = DEF_SWAP_PRIO;

	if (nr + nr_swaptiers > MAX_SWAPTIER) {
		ret = -ERANGE;
		goto out;
	}

	for (int i = 0; i < nr; i++) {
		ret = swap_tier_can_split_range(&desc[i]);
		if (ret)
			goto out;
	}

	for (int i = 0; i < nr; i++)
		swap_tier_prepare(&desc[i]);

	swap_tier_apply(desc, nr);
out:
	spin_unlock(&swap_tier_lock);
	spin_unlock(&swap_lock);

	return ret;
}

static void swap_tier_remove(struct swap_tier *tier)
{
	bool least_prio_merge = false;
	struct plist_node *next;
	struct swap_tier *merge_tier;

	lockdep_assert_held(&swap_tier_lock);

	if (tier->prio_st == DEF_SWAP_PRIO) {
		least_prio_merge = true;
		next = plist_prev(&tier->list);
	} else
		next = plist_next(&tier->list);

	plist_del(&tier->list, &swap_tier_active_list);
	INIT_LIST_HEAD(&tier->inactive_list);
	list_add_tail(&tier->inactive_list, &swap_tier_inactive_list);
	nr_swaptiers--;

	if (nr_swaptiers == SWAP_TIER_RESERVED_CNT)
		return;

	merge_tier = container_of(next, struct swap_tier, list);
	/*
	 * When the first tier is removed, the rule that all priorities
	 * must be covered should be maintained. The next tier inherits the
	 * start value of the removed first tier.
	 */
	if (least_prio_merge)
		merge_tier->prio_st = DEF_SWAP_PRIO;
	else
		merge_tier->prio_end = tier->prio_end;
}

int swap_tiers_remove(struct tiers_desc desc[], int nr)
{
	int cnt = 0;
	int ret = 0;
	struct swap_tier *tier;

	for (int i = 0; i < nr; i++) {
		for (int j = i+1; j < nr; j++) {
			if (!strcmp(desc[i].name, desc[j].name))
				return -EINVAL;
		}
	}

	spin_lock(&swap_tier_lock);
	if (nr <= 0 || nr > nr_swaptiers) {
		ret = -EINVAL;
		goto out;
	}

	for (int i = 0; i < nr; i++) {
		for_each_active_tier(tier) {
			if (swap_tier_is_default(tier))
				continue;

			if (!strcmp(desc[i].name, tier->name)) {
				if (swap_tier_alive(tier)) {
					ret = -EBUSY;
					goto out;
				}
				desc[i].tier_idx = TIER_IDX(tier);
				cnt++;
			}
		}
	}

	if (cnt != nr) {
		ret = -EINVAL;
		goto out;
	}

	for (int i = 0; i < nr; i++)
		swap_tier_put(&swap_tiers[desc[i].tier_idx]);

out:
	spin_unlock(&swap_tier_lock);
	return ret;
}

ssize_t swap_tiers_show_sysfs(char *buf)
{
	struct swap_tier *tier;
	ssize_t len = 0;

	spin_lock(&swap_tier_lock);
	len += sysfs_emit_at(buf, len, "%-16s %-5s %-11s %-11s\n",
			 "Name", "Idx", "PrioStart", "PrioEnd");
	for_each_active_tier(tier) {
		if (swap_tier_is_default(tier))
			len += sysfs_emit_at(buf, len, "%-16s %-5ld\n",
					     tier->name,
					     TIER_IDX(tier));
		else {
			len += sysfs_emit_at(buf, len, "%-16s %-5ld %-11d %-11d\n",
					     tier->name,
					     TIER_IDX(tier),
					     tier->prio_st,
					     tier->prio_end);
		}
		if (len >= PAGE_SIZE)
			break;
	}
	spin_unlock(&swap_tier_lock);

	return len;
}

static void swap_tier_show_mask(struct seq_file *m, int memcg_mask)
{
	struct swap_tier *tier;
	int idx;

	for_each_active_tier(tier) {
		idx = TIER_IDX(tier);
		int tier_on_mask = TIER_MASK(idx, TIER_ON_MASK);
		int tier_off_mask = TIER_MASK(idx, TIER_OFF_MASK);

		if (memcg_mask & tier_on_mask)
			seq_printf(m, "+%s ", tier->name);
		else if (memcg_mask & tier_off_mask)
			seq_printf(m, "-%s ", tier->name);
	}
	seq_puts(m, "\n");
}

static bool swap_tier_has_default(int mask)
{
	return mask & DEFAULT_FULL_MASK;
}

/*
 * TODO: Simplify tier mask collection design if possible.
 *
 * Current approach uses two separate fields per cgroup:
 *   - tiers_onoff: 2-bit per tier to indicate on/off state
 *   - tiers_mask:  2-bit per tier to indicate if the tier is configured
 *
 * Trade-off to consider:
 *   - Pre-compute at cgroup setup time (faster runtime, complex invalidation)
 *   - Keep runtime calculation (simpler, but repeated parent walks)
 */
static int swap_tier_collect_mask(struct mem_cgroup *memcg)
{
	struct mem_cgroup *p;
	int onoff, mask, merge, new;

	if (!memcg)
		return DEFAULT_ON_MASK;

	onoff = memcg->tiers_onoff;
	mask = memcg->tiers_mask;

	rcu_read_lock();
	for (p = parent_mem_cgroup(memcg); p && !swap_tier_has_default(onoff);
		p = parent_mem_cgroup(p)) {
		merge = mask | p->tiers_mask;
		new = merge ^ mask;
		onoff |= p->tiers_onoff & new;
		mask = merge;
	}
	rcu_read_unlock();

	return onoff;
}

int swap_tiers_collect_compare_mask(struct mem_cgroup *memcg)
{
	int onoff;

	onoff = swap_tier_collect_mask(memcg);

	/* Make common case fast */
	if (onoff & DEFAULT_ON_MASK)
		return onoff;
	/*
	 * Root memcg has DEFAULT_ON_MASK; defaults are covered.
	 * Checking DEFAULT_OFF_MASK suffices; only each tier's ON bit is checked.
	 * ON flag is inverted and compared with each swap device's OFF mask.
	 */
	return ~(onoff << 1);
}

void swap_tiers_show_memcg(struct seq_file *m, struct mem_cgroup *memcg)
{
	spin_lock(&swap_tier_lock);
	if (memcg->tiers_onoff)
		swap_tier_show_mask(m, memcg->tiers_onoff);
	else
		seq_puts(m, "\n");
	swap_tier_show_mask(m, swap_tier_collect_mask(memcg));
	spin_unlock(&swap_tier_lock);
}

void swap_tiers_assign(struct swap_info_struct *swp)
{
	struct swap_tier *tier;

	spin_lock(&swap_tier_lock);
	swp->tier_idx = NULL_TIER;

	for_each_active_tier(tier) {
		if (swap_tier_is_default(tier))
			continue;
		if (swap_tier_prio_in_range(tier, swp->prio)) {
			swp->tier_idx = TIER_IDX(tier);
			swap_tier_get(tier);
			break;
		}
	}
	spin_unlock(&swap_tier_lock);
}

void swap_tiers_release(struct swap_info_struct *swp)
{
	spin_lock(&swap_tier_lock);
	if (swp->tier_idx != NULL_TIER)
		swap_tier_put(&swap_tiers[swp->tier_idx]);
	spin_unlock(&swap_tier_lock);
}

/* not incremental, but reset. */
int swap_tiers_get_mask(struct tiers_desc *desc, int nr, struct mem_cgroup *memcg)
{
	struct swap_tier *tier;
	int ret = 0;
	int tiers_mask = 0;
	int tiers_onoff = 0;
	int cnt = 0;

	for (int i = 0; i < nr; i++) {
		for (int j = i+1; j < nr; j++) {
			if (!strcmp(desc[i].name, desc[j].name))
				return -EINVAL;
		}
	}

	spin_lock(&swap_tier_lock);
	/* nr 0 is allowed for deletion */
	if (nr < 0 || nr > nr_swaptiers) {
		ret = -EINVAL;
		goto out;
	}

	for (int i = 0; i < nr; i++) {
		for_each_active_tier(tier) {
			if (!strcmp(desc[i].name, tier->name)) {
				tiers_mask |= TIER_MASK(TIER_IDX(tier), TIER_FULL_MASK);
				tiers_onoff |= TIER_MASK(TIER_IDX(tier), desc[i].ops);
				cnt++;
				break;
			}
		}
	}

	if (cnt != nr) {
		ret = -EINVAL;
		goto out;
	}

	swap_tier_put_mask(memcg->tiers_mask);
	swap_tier_get_mask(tiers_mask);
	memcg->tiers_mask = tiers_mask;
	memcg->tiers_onoff = tiers_onoff;
out:
	spin_unlock(&swap_tier_lock);
	return ret;
}

void swap_tiers_put_mask(struct mem_cgroup *memcg)
{
	spin_lock(&swap_tier_lock);
	swap_tier_put_mask(memcg->tiers_mask);
	spin_unlock(&swap_tier_lock);
}
