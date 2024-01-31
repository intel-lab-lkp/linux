/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LRU_LIST_TYPES_H
#define _LRU_LIST_TYPES_H

#include <linux/list.h>

#ifdef CONFIG_MEMCG_KMEM
#include <linux/types.h> // for bool
#include <linux/xarray_types.h>
#endif

struct list_lru_one {
	struct list_head	list;
	/* may become negative during memcg reparenting */
	long			nr_items;
};

struct list_lru_memcg {
	struct rcu_head		rcu;
	/* array of per cgroup per node lists, indexed by node id */
	struct list_lru_one	node[];
};

struct list_lru_node {
	/* protects all lists on the node, including per cgroup */
	spinlock_t		lock;
	/* global list, used for the root cgroup in cgroup aware lrus */
	struct list_lru_one	lru;
	long			nr_items;
} ____cacheline_aligned_in_smp;

struct list_lru {
	struct list_lru_node	*node;
#ifdef CONFIG_MEMCG_KMEM
	struct list_head	list;
	int			shrinker_id;
	bool			memcg_aware;
	struct xarray		xa;
#endif
};

#endif /* _LRU_LIST_TYPES_H */
