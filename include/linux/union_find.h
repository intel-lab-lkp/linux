/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_UNION_FIND_H
#define __LINUX_UNION_FIND_H

/* Define a union-find node struct */
struct uf_node {
	struct uf_node *parent;
	unsigned int rank;
};

/* Allocate nodes and initialize to 0 */
static inline void uf_nodes_init(struct uf_node *node)
{
	node->parent = node;
	node->rank = 0;
}

/* find the root of a node*/
struct uf_node *uf_find(struct uf_node *node);

/* Merge two intersecting nodes */
void uf_union(struct uf_node *node1, struct uf_node *node2);

#endif /*__LINUX_UNION_FIND_H*/
