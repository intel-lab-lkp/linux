/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Generalized Memory Management.
 *
 * Copyright (C) 2023- Huawei, Inc.
 * Author: Weixi Zhu
 *
 */
#include <linux/mm.h>
#include <linux/gmem.h>

DEFINE_SPINLOCK(hnode_lock);

struct hnode {
	unsigned int id;
	struct gm_dev *dev;
	struct xarray pages;
};

struct hnode *hnodes[MAX_NUMNODES];

static bool is_hnode(int node)
{
	return !node_isset(node, node_possible_map) &&
	       node_isset(node, hnode_map);
}

static bool is_hnode_allowed(int node)
{
	return is_hnode(node) && node_isset(node, current->mems_allowed);
}

static struct hnode *get_hnode(unsigned int hnid)
{
	return hnodes[hnid];
}

void __init hnuma_init(void)
{
	unsigned int node;

	for_each_node(node)
		node_set(node, hnode_map);
}

static unsigned int alloc_hnode_id(void)
{
	unsigned int node;

	spin_lock(&hnode_lock);
	node = first_unset_node(hnode_map);
	node_set(node, hnode_map);
	spin_unlock(&hnode_lock);

	return node;
}

static void free_hnode_id(unsigned int nid)
{
	node_clear(nid, hnode_map);
}

static void hnode_init(struct hnode *hnode, unsigned int hnid,
		       struct gm_dev *dev)
{
	hnodes[hnid] = hnode;
	hnodes[hnid]->id = hnid;
	hnodes[hnid]->dev = dev;
	xa_init(&hnodes[hnid]->pages);
}

static void hnode_deinit(unsigned int hnid, struct gm_dev *dev)
{
	hnodes[hnid]->id = 0;
	hnodes[hnid]->dev = NULL;
	xa_destroy(&hnodes[hnid]->pages);
	hnodes[hnid] = NULL;
}
