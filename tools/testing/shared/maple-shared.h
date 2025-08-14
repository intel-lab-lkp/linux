/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __MAPLE_SHARED_H__
#define __MAPLE_SHARED_H__

#define CONFIG_DEBUG_MAPLE_TREE
#define CONFIG_MAPLE_SEARCH
#define MAPLE_32BIT (MAPLE_NODE_SLOTS > 31)
#include "shared.h"
#include <stdlib.h>
#include <time.h>
#include "linux/init.h"
#include <linux/maple_tree.h>

static inline void free_node(struct rcu_head *head)
{
	struct maple_node *node = container_of(head, struct maple_node, rcu);

	free(node);
}

static inline void kfree_rcu_node(struct maple_node *node)
{
	call_rcu(&node->rcu, free_node);
}

#define kfree_rcu(ptr, memb) kfree_rcu_node(ptr)

#endif /* __MAPLE_SHARED_H__ */
