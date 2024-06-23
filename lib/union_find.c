// SPDX-License-Identifier: GPL-2.0
#include <linux/union_find.h>

struct uf_node *uf_find(struct uf_node *node)
{
	struct uf_node *parent;

	/*Find the root node and perform path compression at the same time*/
	while (node->parent != node) {
		parent = node->parent;
		node->parent = parent->parent;
		node = parent;
	}
	return node;
}

/*Function to merge two sets, using union by rank*/
void uf_union(struct uf_node *node1, struct uf_node *node2)
{
	struct uf_node *root1 = uf_find(node1);
	struct uf_node *root2 = uf_find(node2);

	if (root1 != root2) {
		if (root1->rank < root2->rank) {
			root1->parent = root2;
		} else if (root1->rank > root2->rank) {
			root2->parent = root1;
		} else {
			root2->parent = root1;
			root1->rank++;
		}
	}
}
