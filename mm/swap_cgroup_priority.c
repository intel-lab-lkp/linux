// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 LG Electronics Inc.
 *
 * This file is part of the Linux kernel and implements per-cgroup
 * swap device priority control.
 *
 * This feature allows configuring the preference and exclusion of
 * swap devices on a per-cgroup basis.
 *
 * If no configuration is set, the system-wide swap priorities
 * assigned at swapon time will apply.
 *
 * Author: Youngjun Park <youngjun.park@lge.com>
 */
#include <linux/swap.h>
#include <linux/rcupdate.h>
#include <linux/memcontrol.h>
#include <linux/plist.h>
#include "swap.h"
#include "swap_cgroup_priority.h"
#include "memcontrol-v1.h"

static DEFINE_MUTEX(swap_cgroup_priority_inherit_lck);
static LIST_HEAD(swap_cgroup_priority_list);

/*
 * struct swap_cgroup_priority
 *
 * This structure is RCU protected. Its lifecycle is determined by its
 * owning memcg or when its 'distance' reaches zero. The 'distance' field
 * tracks priority differences from global swap. If zero, and its default_prio
 * follows global swap priority(SWAP_PRIORITY_GLOBAL), the object is destroyed.
 *
 * Child cgroups hold direct pointers to this object for fast access.
 * No reference counting is needed, as the owner's teardown or zero
 * distance directly implies this object's destruction.
 *
 * A child cgroup that has its own effective swap_cgroup_priority uses
 * the 'effective' field to point to the top-most cgroup's relevant
 * swap_cgroup_priority object that it should inherit. Changes in the
 * parent cgroup's swap priority are appropriately propagated downwards.
 *
 * effective - Actual effective swap cgroup priority.
 * pnode - Array of pointers to swap device priority nodes.
 * owner - The owning memory cgroup.
 * rcu - RCU free callback.
 * link - Global linked list entry.
 * least_priority - Current lowest priority.
 * distance - Priority differences from global swap priority.
 * default_prio - Default priority for this cgroup.
 * plist - Priority list head.
 */
struct swap_cgroup_priority {
	struct swap_cgroup_priority *effective;
	struct swap_cgroup_priority_pnode *pnode[MAX_SWAPFILES];
	struct mem_cgroup *owner;

	union {
		struct rcu_head rcu;
		struct list_head link;
	};

	int least_priority;
	s8 distance;
	int default_prio;
	struct plist_head plist[];
};

/*
 * struct swap_cgroup_priority_pnode
 *
 * This structure represents a priority node for a specific swap device
 * within a cgroup.
 *
 * swap - Pointer to the associated swap device.
 * id - Unique identifier for the swap device.
 * prio - Configured priority for this device.
 * avail_lists - Connections to various priority lists.
 */
struct swap_cgroup_priority_pnode {
	struct swap_info_struct *swap;
	u64 id;
	signed short prio;
	struct plist_node avail_lists[];
};

/*
 * Even with a zero distance, a swap device isn't assigned if it doesn't
 * meet global swap priority conditions; thus, we don't clear it.
 */
static bool should_clear_swap_cgroup_priority(
	struct swap_cgroup_priority *swap_priority)
{
	WARN_ON_ONCE(swap_priority->distance < 0 ||
		swap_priority->distance > MAX_SWAPFILES);

	if (swap_priority->distance == 0 &&
	    swap_priority->default_prio == SWAP_PRIORITY_GLOBAL)
		return true;

	return false;
}

/*
 * swapdev_id
 *
 * A unique identifier for a swap device.
 *
 * This ID ensures stable identification for users and crucial synchronization
 * for swap cgroup priority settings. It provides a reliable reference even if
 * device paths or numbers change.
 */
static atomic64_t swapdev_id_counter;

void get_swapdev_id(struct swap_info_struct *si)
{
	si->id = atomic64_inc_return(&swapdev_id_counter);
}

static struct swap_cgroup_priority *get_effective_swap_cgroup_priority(
	struct mem_cgroup *memcg)
{
	struct swap_cgroup_priority *swap_priority;
	if (!memcg)
		return NULL;

	swap_priority = memcg->swap_priority;
	if (!swap_priority)
		return NULL;

	return swap_priority->effective;
}

static bool validate_effective_swap_cgroup_priority(
	struct mem_cgroup *memcg,
	struct swap_cgroup_priority **swap_priority)
{
	struct swap_cgroup_priority *target = memcg->swap_priority;

	if (!target) {
		*swap_priority = NULL;
		return false;
	}

	target = target->effective;
	if (target != *swap_priority) {
		*swap_priority = target;
		return false;
	}

	return true;
}

static struct swap_cgroup_priority_pnode *alloc_swap_cgroup_priority_pnode(
	gfp_t gfp)
{
	struct swap_cgroup_priority_pnode *pnode;
	pnode = kvzalloc(struct_size(pnode, avail_lists, nr_node_ids),
			 gfp);

	return pnode;
}

static void free_swap_cgroup_priority_pnode(
	struct swap_cgroup_priority_pnode *pnode)
{
	if (pnode)
		kvfree(pnode);
}

static void free_swap_cgroup_priority(
	struct swap_cgroup_priority *swap_priority)
{
	for (int i = 0; i < MAX_SWAPFILES; i++)
		free_swap_cgroup_priority_pnode(swap_priority->pnode[i]);

	kvfree(swap_priority);
}

static struct swap_cgroup_priority *alloc_swap_cgroup_priority(void)
{
	struct swap_cgroup_priority *swap_priority;

	swap_priority = kvzalloc(struct_size(swap_priority, plist, nr_node_ids),
				 GFP_KERNEL);
	if (!swap_priority)
		return NULL;

	/*
	 * Pre-allocates pnode array up to nr_swapfiles at init.
	 * Individual pnodes are assigned on swapon, but not freed
	 * on swapoff. This avoids complex ref-counting, simplifying
	 * the structure for swap cgroup priority management.
	 */
	for (int i = 0; i < nr_swapfiles; i++) {
		swap_priority->pnode[i] = alloc_swap_cgroup_priority_pnode(
						GFP_KERNEL);
		if (!swap_priority->pnode[i]) {
			free_swap_cgroup_priority(swap_priority);
			return NULL;
		}

	}

	return swap_priority;
}

static void rcu_free_swap_cgroup_priority(struct rcu_head *rcu)
{
	struct swap_cgroup_priority *swap_priority
		= container_of(rcu, struct swap_cgroup_priority, rcu);

	free_swap_cgroup_priority(swap_priority);
}

void show_swap_cgroup_priority(struct seq_file *m)
{
	struct mem_cgroup *memcg = mem_cgroup_from_seq(m);
	struct swap_cgroup_priority *swap_priority;

	mutex_lock(&swap_cgroup_priority_inherit_lck);
	spin_lock(&swap_lock);

	swap_priority = memcg->swap_priority;
	if (!swap_priority || swap_priority->owner != memcg) {
		spin_unlock(&swap_lock);
		mutex_unlock(&swap_cgroup_priority_inherit_lck);
		return;
	}

	if (swap_priority->default_prio != SWAP_PRIORITY_GLOBAL)
		seq_printf(m,  "default disabled\n");

	for (int i = 0; i < nr_swapfiles; i++) {
		struct swap_info_struct *si = swap_info[i];
		struct swap_cgroup_priority_pnode *pnode;
		signed short prio;

		if (!(si->flags & SWP_USED) || !(si->flags & SWP_WRITEOK))
			continue;

		pnode = swap_priority->pnode[i];

		if (WARN_ON_ONCE(!pnode))
			continue;

		prio = pnode->prio;
		if (prio == si->prio)
			continue;

		seq_printf(m,  "%lld", si->id);
		if (prio != SWAP_PRIORITY_DISABLE)
			seq_printf(m,  " %d\n", prio);
		else
			seq_printf(m,  " disabled\n");
	}

	spin_unlock(&swap_lock);
	mutex_unlock(&swap_cgroup_priority_inherit_lck);
}

void show_swap_cgroup_priority_effective(struct seq_file *m)
{
	struct mem_cgroup *memcg = mem_cgroup_from_seq(m);
	struct swap_cgroup_priority *swap_priority;

	mutex_lock(&swap_cgroup_priority_inherit_lck);
	spin_lock(&swap_lock);

	swap_priority = get_effective_swap_cgroup_priority(memcg);
	if (swap_priority && swap_priority->default_prio != SWAP_PRIORITY_GLOBAL)
		seq_printf(m,  "default disabled\n");

	for (int i = 0; i < nr_swapfiles; i++) {
		struct swap_info_struct *si = swap_info[i];
		struct swap_cgroup_priority_pnode *pnode;
		signed short prio;

		if (!(si->flags & SWP_USED) || !(si->flags & SWP_WRITEOK))
			continue;

		seq_printf(m,  "%lld", si->id);
		if (!swap_priority) {
			seq_printf(m, " %d\n", si->prio);
			continue;
		}

		pnode = swap_priority->pnode[i];
		if (WARN_ON(!pnode))
			continue;

		prio = pnode->prio;
		if (prio != SWAP_PRIORITY_DISABLE)
			seq_printf(m,  " %d\n", prio);
		else
			seq_printf(m,  " disabled\n");
	}
	spin_unlock(&swap_lock);
	mutex_unlock(&swap_cgroup_priority_inherit_lck);
}

static void __delete_swap_cgroup_priority(struct mem_cgroup *memcg);
void purge_swap_cgroup_priority(void)
{
	struct swap_cgroup_priority *swap_priority, *tmp;

	mutex_lock(&swap_cgroup_priority_inherit_lck);
	spin_lock(&swap_avail_lock);
	list_for_each_entry_safe(swap_priority, tmp, &swap_cgroup_priority_list,
				 link) {

		if (should_clear_swap_cgroup_priority(swap_priority))
			__delete_swap_cgroup_priority(swap_priority->owner);
	}
	spin_unlock(&swap_avail_lock);
	mutex_unlock(&swap_cgroup_priority_inherit_lck);
}

bool swap_alloc_cgroup_priority(struct mem_cgroup *memcg,
				swp_entry_t *entry, int order)
{
	struct swap_cgroup_priority *swap_priority;
	struct swap_cgroup_priority_pnode *pnode, *next;
	struct swap_info_struct *si;
	unsigned long offset;
	int node;

	/*
	 * TODO: Per-cpu swap cluster cache can't be used directly
	 * as cgroup-specific priorities may select different devices.
	 */
	spin_lock(&swap_avail_lock);
	node = numa_node_id();

	swap_priority = get_effective_swap_cgroup_priority(memcg);
swap_priority_check:
	if (!swap_priority) {
		spin_unlock(&swap_avail_lock);
		return false;
	}

start_over:
	plist_for_each_entry_safe(pnode, next, &swap_priority->plist[node],
				  avail_lists[node]) {
		si = pnode->swap;
		plist_requeue(&pnode->avail_lists[node],
			&swap_priority->plist[node]);
		spin_unlock(&swap_avail_lock);
		if (get_swap_device_info(si)) {
			offset = cluster_alloc_swap_entry(si, order,
							  SWAP_HAS_CACHE);
			put_swap_device(si);
			if (offset) {
				*entry = swp_entry(si->type, offset);
				return true;
			}

			if (order)
				return false;
		}

		spin_lock(&swap_avail_lock);
		/*
		 * If 'swap_cgroup_priority' changes while we're holding a lock,
		 * we must verify its state to ensure memory validness.
		 */
		if (!validate_effective_swap_cgroup_priority(memcg,
							     &swap_priority))
			goto swap_priority_check;

		if (plist_node_empty(&next->avail_lists[node]))
			goto start_over;
	}
	spin_unlock(&swap_avail_lock);

	return false;
}

/* add_to_avail_list (swapon / swapusage > 0) */
void activate_swap_cgroup_priority(struct swap_info_struct *swp,
				   bool swapon)
{
	struct swap_cgroup_priority *swap_priority;
	int i;

	list_for_each_entry(swap_priority, &swap_cgroup_priority_list, link) {
		struct swap_cgroup_priority_pnode *pnode =
			swap_priority->pnode[swp->type];

		if (WARN_ON_ONCE(!pnode))
			continue;

		/* Exclude reinsert */
		if (swapon && pnode->id != swp->id) {
			pnode->swap = swp;
			if (swap_priority->default_prio == SWAP_PRIORITY_GLOBAL) {
				if (swp->prio >= 0)
					pnode->prio = swp->prio;
				else
					pnode->prio =
						--swap_priority->least_priority;
			} else {
				pnode->prio = SWAP_PRIORITY_DISABLE;
				swap_priority->distance++;
			}
		}

		/* NUMA priority handling */
		for_each_node(i) {
			if (swapon) {
				if (pnode->prio < 0 && swap_node(swp) == i) {
					plist_node_init(
						&pnode->avail_lists[i],
						1);
				} else {
					plist_node_init(
						&pnode->avail_lists[i],
						-pnode->prio);
				}
			}

			if (pnode->prio != SWAP_PRIORITY_DISABLE)
				plist_add(&pnode->avail_lists[i],
					  &swap_priority->plist[i]);
		}
	}
}

/* del_from_avail_list (swapoff / swap usage <= 0) */
void deactivate_swap_cgroup_priority(struct swap_info_struct *swp,
				     bool swapoff)
{
	struct swap_cgroup_priority *swap_priority, *tmp;
	int nid, i;

	list_for_each_entry_safe(swap_priority, tmp, &swap_cgroup_priority_list, 
				 link) {
		struct swap_cgroup_priority_pnode *pnode =
			swap_priority->pnode[swp->type];

		if (WARN_ON_ONCE(!pnode))
			continue;

		if (swapoff) {
			if (pnode->prio != swp->prio)
				swap_priority->distance--;
		}

		if (pnode->prio == SWAP_PRIORITY_DISABLE)
			continue;

		if (swapoff && pnode->prio < 0) {
			struct swap_cgroup_priority_pnode *tmp;
			/*
			 * NUMA priority handling
			 * mimic swapoff prio adjustment without plist
			 */
			for (int i = 0; i < nr_swapfiles; i++) {
				tmp = swap_priority->pnode[i];
				if (!tmp || tmp->prio > pnode->prio ||
				    tmp->swap == swp)
					continue;
				tmp->prio++;
				for_each_node(nid) {
					if (tmp->avail_lists[nid].prio != 1)
						tmp->avail_lists[nid].prio--;
				}
			}

			swap_priority->least_priority++;
		}

		for_each_node(i)
			plist_del(&pnode->avail_lists[i],
				&swap_priority->plist[i]);
	}
}

static void apply_swap_cgroup_priority_pnode(
	struct swap_cgroup_priority *swap_priority,
	struct swap_cgroup_priority_pnode *pnode,
	int prio,
	bool clear)
{
	int nid;

	if (clear && pnode->prio != SWAP_PRIORITY_DISABLE) {
		for_each_node(nid) {
			plist_del(&pnode->avail_lists[nid],
				&swap_priority->plist[nid]);
		}
	}

	if (pnode->swap->prio != prio && pnode->swap->prio == pnode->prio)
		swap_priority->distance++;
	else if (pnode->swap->prio == prio && pnode->swap->prio != pnode->prio)
		swap_priority->distance--;

	pnode->prio = prio;
	for_each_node(nid) {
		if (pnode->prio >= 0) {
			plist_node_init(&pnode->avail_lists[nid],
				-pnode->prio);
		} else {
			if (swap_node(pnode->swap) == nid)
				plist_node_init(
					&pnode->avail_lists[nid],
					1);
			else
				plist_node_init(
					&pnode->avail_lists[nid],
					-pnode->prio);
		}

		/*
		 * Check SWP_WRITEOK for skipping
		 * 1. reinsert case when swapoff fails
		 * 2. on-going swapon before adding avail list
		 */
		if (pnode->prio != SWAP_PRIORITY_DISABLE &&
		    (pnode->swap->flags & SWP_WRITEOK))
			plist_add(&pnode->avail_lists[nid],
				&swap_priority->plist[nid]);
	}
}

static int __apply_swap_cgroup_priority(
	struct swap_cgroup_priority *swap_priority, u64 id, int prio, bool new)
{
	struct swap_cgroup_priority_pnode *pnode;
	struct swap_info_struct *si;
	int old_prio;
	int type;

	if (new)
		swap_priority->least_priority = least_priority;

	if (id == DEFAULT_ID) {
		swap_priority->default_prio = prio;
		if (new)
			goto assign_prio;

		goto out;
	}

	for (type = 0; type < nr_swapfiles; type++) {
		si = swap_info[type];
		if (id == si->id)
			break;
		si = NULL;
	}

	if (!si)
		return -EIO;

	if (!(si->flags & SWP_USED) || !(si->flags & SWP_WRITEOK))
		return -EFAULT;

	if (si->id != id)
		return -EINVAL;

	if (prio == SWAP_PRIORITY_GLOBAL)
		prio = si->prio;

	pnode = swap_priority->pnode[type];
	/* Assigning the same priority has no effect. */
	if (!new && pnode && pnode->prio == prio)
		return 0;
	else if (new && si->prio == prio)
		return 0;

	if (new) {
		pnode->id = id;
		pnode->swap = si;
		pnode->prio = si->prio;
	}
	old_prio = pnode->prio;

	/*
	 * When a new negative priority is added, least_priority decreases.
	 * When a negative priority is deleted, least_priority increases.
	 */
	if (prio < SWAP_PRIORITY_DISABLE && old_prio >= SWAP_PRIORITY_DISABLE)
		swap_priority->least_priority--;
	else if (prio >= SWAP_PRIORITY_DISABLE &&
		 old_prio < SWAP_PRIORITY_DISABLE)
		swap_priority->least_priority++;

	if (prio < swap_priority->least_priority)
		prio = swap_priority->least_priority;

	apply_swap_cgroup_priority_pnode(swap_priority, pnode, prio, !new);

	/*
	 * This logic adjusts priorities according to global swap on/off rule.
	 * Priorities at or above SWAP_PRIORITY_DISABLE don't affect other swap
	 * device priorities. However, negative priorities below this threshold
	 * influence each other based on their values. Adjustments are made if a
	 * swap device's priority becomes negative and starts influencing others,
	 * or if it moves out of the negative range and stops influencing them.
	 */
assign_prio:
	for (int i = 0; i < nr_swapfiles; i++) {
		int changed_prio;
		si = swap_info[i];
		/*
		 * nr_swapfiles may have increased after initial alloc
		 * due to missing swap_lock
		 */
		if (!(pnode = swap_priority->pnode[si->type])) {
			pnode = alloc_swap_cgroup_priority_pnode(GFP_ATOMIC);
			if (!pnode)
				return -ENOMEM;
			swap_priority->pnode[si->type] = pnode;
		}

		/*
		 * Does not check SWP_WRITEOK. device could be reinserted.
		 * Ensure si->map is valid before proceeding.
		 * This prevents missing swapon failures where SWP_USED
		 * state persists unexpectedly.
		 */
		if (!(si->flags & SWP_USED) || !si->swap_map)
			continue;

		if (si->id == id)
			continue;

		if (si->id != pnode->id) {
			pnode->id = si->id;
			pnode->prio = si->prio;
			pnode->swap = si;
		}

		changed_prio = pnode->prio;

		/*
		 * A new negative value is added,
		 * so all values lower than it are shifted backward by one.
		 */
		if (old_prio >= SWAP_PRIORITY_DISABLE &&
		    prio < SWAP_PRIORITY_DISABLE &&
		    (pnode->prio < SWAP_PRIORITY_DISABLE &&
		    pnode->prio <= prio)) {
			changed_prio--;
		/*
		 * One negative value is removed,
		 * so all higher values are shifted forward by one.
		 */
		} else if (old_prio < SWAP_PRIORITY_DISABLE &&
			   prio >= SWAP_PRIORITY_DISABLE &&
			   (pnode->prio < SWAP_PRIORITY_DISABLE &&
			   pnode->prio <= old_prio)) {
			changed_prio++;
		} else if (old_prio < SWAP_PRIORITY_DISABLE &&
			   prio < SWAP_PRIORITY_DISABLE) {
			/*
			 * If it was negative already but becomes smaller,
			 * shift all values in range backward by one.
			 */
			if (old_prio > prio &&
			    (prio <= pnode->prio && old_prio >= pnode->prio)) {
				changed_prio++;
			/*
			 * If it was negative already but becomes larger,
			 * shift all values in range forward by one.
			 */
			} else if (old_prio < prio &&
				   (prio >= pnode->prio &&
				   old_prio <= pnode->prio)) {
				changed_prio--;
			}
		}

		if (!new && changed_prio == pnode->prio)
			continue;

		apply_swap_cgroup_priority_pnode(
			swap_priority, pnode, changed_prio, !new);
	}

out:
	if (should_clear_swap_cgroup_priority(swap_priority))
		return 1;

	return 0;
}

/*
 * If this is the top-level swap_cgroup_priority, propagation is needed.
 * We traverse the 'mem_cgroup_tree' using 'for_each_mem_cgroup_tree'.
 * Due to its pre-order traversal, after propagating changes in the parent,
 * subsequent child nodes can correctly retrieve the parent's effective
 * swap_cgroup_priority, ensuring proper propagation.
 */
static void propagate_swap_cgroup_priority(
	struct mem_cgroup *memcg,
	struct swap_cgroup_priority *swap_priority)
{
	struct mem_cgroup *iter;

	iter = parent_mem_cgroup(memcg);
	while (iter) {
		if (iter->swap_priority)
			return;
		iter = parent_mem_cgroup(iter);
	}

	for_each_mem_cgroup_tree(iter, memcg) {
		if (iter == memcg)
			continue;

		if (iter->swap_priority &&
			iter->swap_priority->owner == iter) {
			rcu_assign_pointer(iter->swap_priority->effective,
					   swap_priority ?
					   swap_priority : iter->swap_priority);
		} else {
			struct swap_cgroup_priority *effective =
				get_effective_swap_cgroup_priority(
					parent_mem_cgroup(iter));
			iter->swap_priority = effective;
		}
	}

	return;
}

int prepare_swap_cgroup_priority(int type)
{
	struct swap_cgroup_priority *swap_priority;
	int err = 0;

	mutex_lock(&swap_cgroup_priority_inherit_lck);
	list_for_each_entry_rcu(swap_priority,
				&swap_cgroup_priority_list, link) {
		if (!swap_priority->pnode[type]) {
			swap_priority->pnode[type] =
				alloc_swap_cgroup_priority_pnode(GFP_KERNEL);

			if (!swap_priority->pnode[type]) {
				err = -ENOMEM;
				break;
			}
		}

	}
	mutex_unlock(&swap_cgroup_priority_inherit_lck);

	return err;
}

struct swap_cgroup_priority *inherit_swap_cgroup_priority(
	struct mem_cgroup *parent)
{
	struct swap_cgroup_priority *swap_priority;

	mutex_lock(&swap_cgroup_priority_inherit_lck);
	swap_priority = get_effective_swap_cgroup_priority(parent);
	mutex_unlock(&swap_cgroup_priority_inherit_lck);

	return swap_priority;
}

int apply_swap_cgroup_priority(struct mem_cgroup *memcg, u64 id, int prio)
{
	struct swap_cgroup_priority *swap_priority;
	int nid;
	bool new = false;
	int err = 0;

	mutex_lock(&swap_cgroup_priority_inherit_lck);
	swap_priority = memcg->swap_priority;
	if (swap_priority && swap_priority->owner == memcg)
		goto prio_set;

	new = true;

	/* No need to define "global swap priority" for a new cgroup. */
	if (new && prio == SWAP_PRIORITY_GLOBAL) {
		mutex_unlock(&swap_cgroup_priority_inherit_lck);
		return 0;
	}

	swap_priority = alloc_swap_cgroup_priority();
	if (!swap_priority) {
		mutex_unlock(&swap_cgroup_priority_inherit_lck);
		return -ENOMEM;
	}

	/* Just initialize. may changed on __apply_swap_cgroup_priority */
	swap_priority->default_prio = SWAP_PRIORITY_GLOBAL;
	INIT_LIST_HEAD(&swap_priority->link);
	for_each_node(nid)
		plist_head_init(&swap_priority->plist[nid]);

prio_set:
	spin_lock(&swap_lock);
	spin_lock(&swap_avail_lock);

	err = __apply_swap_cgroup_priority(swap_priority, id, prio, new);
	if (err) {
		/*
		 * The difference with the global swap priority is now zero.
		 * Remove the swap priority, and propagate if needed.
		 */
		if (err == 1) {
			err = 0;
			__delete_swap_cgroup_priority(memcg);
			if (swap_priority != swap_priority->effective)
				memcg->swap_priority = swap_priority->effective;
		}

		goto error_locked;
	}

	if (new) {
		swap_priority->owner = memcg;
		list_add_rcu(&swap_priority->link, &swap_cgroup_priority_list);
	        /* If there was an inherited swap priority, update effective. */
		if (memcg->swap_priority) {
			swap_priority->effective = memcg->swap_priority;
			memcg->swap_priority = swap_priority;
		} else {
			swap_priority->effective = swap_priority;
			memcg->swap_priority = swap_priority;
	                /*
			 * Might be a top-level parent memcg,
			 * so propagate effective priority.
			 */
			propagate_swap_cgroup_priority(memcg, swap_priority);
		}

		for (int i = 0; i < nr_swapfiles; i++) {
			if (!swap_priority->pnode[i]->swap) {
				free_swap_cgroup_priority_pnode(
					swap_priority->pnode[i]);
				swap_priority->pnode[i] = NULL;
			}
		}
	}

	spin_unlock(&swap_avail_lock);
	spin_unlock(&swap_lock);
	mutex_unlock(&swap_cgroup_priority_inherit_lck);
	return 0;

error_locked:
	spin_unlock(&swap_avail_lock);
	spin_unlock(&swap_lock);
	mutex_unlock(&swap_cgroup_priority_inherit_lck);

	if (!new)
		return err;

	free_swap_cgroup_priority(swap_priority);
	return err;
}

static void __delete_swap_cgroup_priority(struct mem_cgroup *memcg)
{
	struct swap_cgroup_priority *swap_priority = memcg->swap_priority;

	lockdep_assert_held(&swap_cgroup_priority_inherit_lck);
	lockdep_assert_held(&swap_avail_lock);

	if (!swap_priority)
		return;

	/* If using a cached swap_priority, there is no need to remove it. */
	if (swap_priority->owner != memcg)
		return;

	rcu_assign_pointer(memcg->swap_priority, NULL);
	propagate_swap_cgroup_priority(memcg, NULL);
	list_del_rcu(&swap_priority->link);
	call_rcu(&swap_priority->rcu, rcu_free_swap_cgroup_priority);
}

void delete_swap_cgroup_priority(struct mem_cgroup *memcg)
{
	mutex_lock(&swap_cgroup_priority_inherit_lck);
	spin_lock(&swap_avail_lock);
	__delete_swap_cgroup_priority(memcg);
	spin_unlock(&swap_avail_lock);
	mutex_unlock(&swap_cgroup_priority_inherit_lck);
}
