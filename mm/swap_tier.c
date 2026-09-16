// SPDX-License-Identifier: GPL-2.0
#include <linux/swap.h>
#if defined(CONFIG_MEMCG) && defined(CONFIG_DEBUG_FS)
#include <linux/debugfs.h>
#include <linux/memcontrol.h>
#include <linux/seq_file.h>
#endif

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

/* A tier's index is its slot in the array, stable for its lifetime. */
#define TIER_IDX(tier)	((tier) - swap_tiers)
#define TIER_MASK(tier)	(1U << TIER_IDX(tier))

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

#if defined(CONFIG_MEMCG) && defined(CONFIG_DEBUG_FS)
static void swap_tiers_debugfs_init(void);
#else
static inline void swap_tiers_debugfs_init(void) {}
#endif

void swap_tiers_init(void)
{
	struct swap_tier *tier;
	int idx;

	BUILD_BUG_ON(BITS_PER_TYPE(int) < MAX_SWAPTIER);

	for_each_tier(tier, idx) {
		plist_head_init(&tier->active_head);
		plist_head_init(&tier->avail_head);
		INIT_LIST_HEAD(&tier->list);
		swap_tier_inactivate(tier);
	}

	swap_tiers_debugfs_init();
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

	/* The allocator walks the tiers under swap_avail_lock. */
	spin_lock(&swap_avail_lock);
	tier = swap_tier_lookup(swp->prio);
	if (!tier) {
		tier = swap_tier_prepare(swp->prio);
		swap_tier_activate(tier);
	}
	spin_unlock(&swap_avail_lock);

	plist_add(&swp->list, &tier->active_head);

	/* Put back by a failed swapoff, so already stamped and counted. */
	if (swp->tier_mask)
		return;

	/*
	 * A device's tier never changes, so stamp it once here. Paired with
	 * the READ_ONCE() in the allocator, which reads this without swap_lock.
	 */
	tier->nr_devs++;
	WRITE_ONCE(swp->tier_mask, TIER_MASK(tier));
}

void swap_tiers_remove_dev(struct swap_info_struct *swp)
{
	struct swap_tier *tier;

	lockdep_assert_held(&swap_lock);

	tier = swap_tier_lookup(swp->prio);
	plist_del(&swp->list, &tier->active_head);
}

/*
 * A failed swapoff puts the device back into its tier, so the tier is given
 * up only here, once swapoff can no longer fail. Returns the tier's mask if
 * @swp was its last device, 0 otherwise.
 */
unsigned int swap_tiers_release_dev(struct swap_info_struct *swp)
{
	struct swap_tier *tier;
	unsigned int freed = 0;

	lockdep_assert_held(&swap_lock);

	tier = swap_tier_lookup(swp->prio);
	if (!--tier->nr_devs) {
		spin_lock(&swap_avail_lock);
		swap_tier_inactivate(tier);
		spin_unlock(&swap_avail_lock);
		freed = TIER_MASK(tier);
	}

	WRITE_ONCE(swp->tier_mask, 0);

	return freed;
}

/* The avail list of the tier @swp belongs to. */
struct plist_head *swap_tiers_avail_head(struct swap_info_struct *swp)
{
	lockdep_assert_held(&swap_avail_lock);

	return &swap_tier_lookup(swp->prio)->avail_head;
}

#if defined(CONFIG_MEMCG) && defined(CONFIG_DEBUG_FS)
static DEFINE_MUTEX(swap_tier_lock);

/*
 * struct swap_tier_cgroup - tier mask of a cgroup.
 *
 * @id: cgroup ID of the cgroup.
 * @mask: tiers the cgroup may swap to.
 * @list: linkage into swap_tier_cgroup_list.
 * @rcu: frees the entry after a grace period.
 */
struct swap_tier_cgroup {
	u64 id;
	unsigned int mask;
	struct list_head list;
	struct rcu_head rcu;
};

/*
 * Cgroups written to memcg_tiers. Changed under swap_tier_lock, walked by
 * the allocator under RCU. A cgroup not on the list may use every tier.
 */
static LIST_HEAD(swap_tier_cgroup_list);

static struct swap_tier_cgroup *swap_tier_cgroup_lookup(u64 id)
{
	struct swap_tier_cgroup *stc;

	list_for_each_entry_rcu(stc, &swap_tier_cgroup_list, list,
				lockdep_is_held(&swap_tier_lock)) {
		if (stc->id == id)
			return stc;
	}

	return NULL;
}

/* Drop the entries that allow every tier or whose cgroup is removed. */
static void swap_tier_cgroup_prune(void)
{
	struct swap_tier_cgroup *stc, *tmp;
	struct cgroup *cgrp;

	lockdep_assert_held(&swap_tier_lock);

	list_for_each_entry_safe(stc, tmp, &swap_tier_cgroup_list, list) {
		if (stc->mask != TIER_ALL_MASK) {
			cgrp = __cgroup_get_from_id(stc->id);
			if (!IS_ERR(cgrp)) {
				cgroup_put(cgrp);
				continue;
			}
		}

		list_del_rcu(&stc->list);
		kfree_rcu(stc, rcu);
	}
}

/* One line per cgroup that dropped a tier, in the syntax a write takes. */
static int swap_tiers_memcg_show(struct seq_file *m, void *v)
{
	struct swap_tier_cgroup *stc;
	struct cgroup *cgrp;
	char *path;

	path = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	mutex_lock(&swap_tier_lock);
	list_for_each_entry(stc, &swap_tier_cgroup_list, list) {
		if (stc->mask == TIER_ALL_MASK)
			continue;

		/* A removed cgroup's entry stays until the next write. */
		cgrp = __cgroup_get_from_id(stc->id);
		if (IS_ERR(cgrp))
			continue;

		cgroup_path(cgrp, path, PATH_MAX);
		cgroup_put(cgrp);
		seq_printf(m, "%s 0x%x\n", path, stc->mask);
	}
	mutex_unlock(&swap_tier_lock);

	kfree(path);
	return 0;
}

/*
 * Keep only the tiers set in @mask.  @cgpath must name a cgroup that has
 * the memory controller enabled.
 */
static int swap_tiers_memcg_set(const char *cgpath, unsigned int mask)
{
	struct swap_tier_cgroup *stc;
	struct cgroup *cgrp;
	bool enabled;
	int ret = 0;
	u64 id;

	cgrp = cgroup_get_from_path(cgpath);
	if (IS_ERR(cgrp))
		return PTR_ERR(cgrp);

	/*
	 * Not cgroup_get_e_css(), which falls back to an ancestor when the
	 * memory controller is not enabled here.
	 */
	rcu_read_lock();
	enabled = cgroup_css(cgrp, &memory_cgrp_subsys);
	rcu_read_unlock();
	id = cgroup_id(cgrp);
	cgroup_put(cgrp);

	if (!enabled)
		return -ENOENT;

	mutex_lock(&swap_tier_lock);

	stc = swap_tier_cgroup_lookup(id);
	if (stc) {
		WRITE_ONCE(stc->mask, mask);
	} else if (mask != TIER_ALL_MASK) {
		stc = kmalloc_obj(*stc, GFP_KERNEL);
		if (stc) {
			stc->id = id;
			stc->mask = mask;
			list_add_rcu(&stc->list, &swap_tier_cgroup_list);
		} else {
			ret = -ENOMEM;
		}
	}
	swap_tier_cgroup_prune();

	mutex_unlock(&swap_tier_lock);
	return ret;
}

/* The tiers that the cgroup @folio is charged to may swap to. */
unsigned int folio_tier_mask(struct folio *folio)
{
	struct swap_tier_cgroup *stc;
	struct mem_cgroup *memcg;
	unsigned int mask = TIER_ALL_MASK;

	rcu_read_lock();
	memcg = folio_memcg(folio);
	if (memcg) {
		stc = swap_tier_cgroup_lookup(cgroup_id(memcg->css.cgroup));
		if (stc)
			mask = READ_ONCE(stc->mask);
	}
	rcu_read_unlock();

	return mask;
}

/*
 * When a tier is removed, its index (bit position in the mask) becomes
 * free for reassignment to a future tier. If a cgroup had previously
 * disabled this tier (cleared the bit in its memcg_tiers entry), its mask
 * would keep that bit clear, meaning the new tier at the same index would
 * be silently unavailable, an invisible cgroup constraint left behind by a
 * tier that no longer exists.
 *
 * To prevent this, OR the removed tier's mask bit into every cgroup's
 * mask. This resets the bit so the new tier is accessible by default.
 * Users who want to restrict it must explicitly disable it after the tier
 * is re-created.
 */
void swap_tiers_memcg_propagate(unsigned int mask)
{
	struct swap_tier_cgroup *stc;

	mutex_lock(&swap_tier_lock);
	list_for_each_entry(stc, &swap_tier_cgroup_list, list)
		WRITE_ONCE(stc->mask, stc->mask | mask);
	mutex_unlock(&swap_tier_lock);
}

static int swap_tiers_memcg_open(struct inode *inode, struct file *file)
{
	return single_open(file, swap_tiers_memcg_show, NULL);
}

static ssize_t swap_tiers_memcg_write(struct file *file,
				      const char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	char *pos, *tmp, *cgpath;
	unsigned int mask;
	int ret;

	tmp = memdup_user_nul(ubuf, count);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	pos = strstrip(tmp);
	cgpath = strsep(&pos, " \t\n");
	if (!cgpath || !*cgpath || !pos ||
	    kstrtouint(skip_spaces(pos), 16, &mask))
		ret = -EINVAL;
	else
		ret = swap_tiers_memcg_set(cgpath, mask);

	kfree(tmp);
	return ret ? ret : count;
}

static const struct file_operations swap_tiers_memcg_fops = {
	.open		= swap_tiers_memcg_open,
	.read		= seq_read,
	.write		= swap_tiers_memcg_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int swap_tiers_show(struct seq_file *m, void *v)
{
	struct swap_tier *tier;

	seq_printf(m, "%-5s %s\n", "Idx", "Prio");

	spin_lock(&swap_lock);
	for_each_active_tier(tier)
		seq_printf(m, "%-5td %d\n", TIER_IDX(tier), tier->prio);
	spin_unlock(&swap_lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(swap_tiers);

static void swap_tiers_debugfs_init(void)
{
	struct dentry *dir = debugfs_create_dir("swap", NULL);

	debugfs_create_file("tiers", 0400, dir, NULL, &swap_tiers_fops);
	debugfs_create_file("memcg_tiers", 0600, dir, NULL,
			    &swap_tiers_memcg_fops);
}
#endif
