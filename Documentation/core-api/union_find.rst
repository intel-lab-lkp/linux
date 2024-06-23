.. SPDX-License-Identifier: GPL-2.0

====================
Union-Find in Linux
====================


:Date: June 21, 2024
:Author: Xavier <xavier_qy@163.com>

What is Union-Find, and what is it used for?
------------------------------------------------

Union-Find is a data structure used to handle the merging and querying
of disjoint sets. The primary operations supported by Union-Find are:

	Initialization: Resetting each element as an individual set, with
		each set's initial parent node pointing to itself.
	Find: Determine which set a particular element belongs to, usually by
		returning a “representative element” of that set. This operation
		is used to check if two elements are in the same set.
	Union: Merge two sets into one.

As a data structure used to maintain sets (groups), Union-Find is commonly
utilized to solve problems related to offline queries, dynamic connectivity,
and graph theory. It is also a key component in Kruskal's algorithm for
computing the minimum spanning tree, which is crucial in scenarios like
network routing. Consequently, Union-Find is widely referenced. Additionally,
Union-Find has applications in symbolic computation, register allocation,
and more.

Space Complexity: O(n), where n is the number of nodes.

Time Complexity: Using path compression can reduce the time complexity of
the find operation, and using union by rank can reduce the time complexity
of the union operation. These optimizations reduce the average time
complexity of each find and union operation to O(α(n)), where α(n) is the
inverse Ackermann function. This can be roughly considered a constant time
complexity for practical purposes.

This document covers use of the Linux union-find implementation.  For more
information on the nature and implementation of Union-Find,  see:

  Wikipedia entry on union-find
    https://en.wikipedia.org/wiki/Disjoint-set_data_structure

Linux implementation of union-find
-----------------------------------

Linux's union-find implementation resides in the file "lib/union_find.c".
To use it, "#include <linux/union_find.h>".

The Union-Find data structure is defined as follows::

	struct uf_node {
		struct uf_node *parent;
		unsigned int rank;
	};

In this structure, parent points to the parent node of the current node.
The rank field represents the height of the current tree. During a union
operation, the tree with the smaller rank is attached under the tree with the
larger rank to maintain balance.

Initializing Union-Find
--------------------

When initializing the Union-Find data structure, a single pointer to the
Union-Find instance needs to be passed. Initialize the parent pointer to point
to itself and set the rank to 0.
Example::

	struct uf_node *my_node = vzalloc(sizeof(struct uf_node));
	uf_nodes_init(my_node);

Find the Root Node of Union-Find
--------------------------------

This operation is mainly used to determine whether two nodes belong to the same
set in the Union-Find. If they have the same root, they are in the same set.
During the find operation, path compression is performed to improve the
efficiency of subsequent find operations.
Example::

	int connected;
	struct uf_node *root1 = uf_find(&my_node[0]);
	struct uf_node *root2 = uf_find(&my_node[1]);
	if (root1 == root2)
		connected = 1;
	else
		connected = 0;

Union Two Sets in Union-Find
----------------------------

To union two sets in the Union-Find, you first find their respective root nodes
and then link the smaller node to the larger node based on the rank of the root
nodes.
Example::

	uf_union(&my_node[0], &my_node[1]);
