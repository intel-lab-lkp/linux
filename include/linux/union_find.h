/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_UNION_FIND_H
#define __LINUX_UNION_FIND_H
#include <linux/vmalloc.h>

/* Define a union-find node struct */
struct uf_node {
	struct uf_node *parent;
	unsigned int rank;
};

/* Allocate nodes and initialize to 0 */
static inline struct uf_node *uf_nodes_alloc(unsigned int node_num)
{
	return vzalloc(sizeof(struct uf_node) * node_num);
}

/* Free nodes*/
static inline void uf_nodes_free(struct uf_node *nodes)
{
	vfree(nodes);
}

/* find the root of a node*/
struct uf_node *uf_find(struct uf_node *node);

/* Merge two intersecting nodes */
void uf_union(struct uf_node *node1, struct uf_node *node2);

#endif /*__LINUX_UNION_FIND_H*/
