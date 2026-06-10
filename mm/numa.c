// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/memblock.h>
#include <linux/printk.h>
#include <linux/numa.h>
#include <linux/numa_memblks.h>
#include <linux/spinlock.h>
#include <linux/export.h>

struct pglist_data *node_data[MAX_NUMNODES];
EXPORT_SYMBOL(node_data);

/* Allocate NODE_DATA for a node on the local memory */
void __init alloc_node_data(int nid)
{
	const size_t nd_size = roundup(sizeof(pg_data_t), SMP_CACHE_BYTES);
	u64 nd_pa;
	int tnid;

	/* Allocate node data.  Try node-local memory and then any node. */
	nd_pa = memblock_phys_alloc_try_nid(nd_size, SMP_CACHE_BYTES, nid);
	if (!nd_pa)
		panic("Cannot allocate %zu bytes for node %d data\n",
		      nd_size, nid);

	/* report and initialize */
	pr_info("NODE_DATA(%d) allocated [mem %#010Lx-%#010Lx]\n", nid,
		nd_pa, nd_pa + nd_size - 1);
	tnid = early_pfn_to_nid(nd_pa >> PAGE_SHIFT);
	if (tnid != nid)
		pr_info("    NODE_DATA(%d) on node %d\n", nid, tnid);

	node_data[nid] = __va(nd_pa);
	memset(NODE_DATA(nid), 0, sizeof(pg_data_t));
}

void __init alloc_offline_node_data(int nid)
{
	pg_data_t *pgdat;
	node_data[nid] = memblock_alloc_or_panic(sizeof(*pgdat), SMP_CACHE_BYTES);
}

/* Stub functions: */

#ifndef memory_add_physaddr_to_nid
int memory_add_physaddr_to_nid(u64 start)
{
	pr_info_once("Unknown online node for memory at 0x%llx, assuming node 0\n",
			start);
	return 0;
}
EXPORT_SYMBOL_GPL(memory_add_physaddr_to_nid);
#endif

#ifndef phys_to_target_node
int phys_to_target_node(u64 start)
{
	pr_info_once("Unknown target node for memory at 0x%llx, assuming node 0\n",
			start);
	return 0;
}
EXPORT_SYMBOL_GPL(phys_to_target_node);
#endif

/*
 * Pool of exclusive NUMA nodes available for runtime claiming.
 *
 * Published by numa_commit_standby_nodes() from standby nodes staged
 * during __init. Protected by exclusive_node_lock at runtime.
 */
static nodemask_t exclusive_nodes = NODE_MASK_NONE;
static DEFINE_SPINLOCK(exclusive_node_lock);

/*
 * Standby node candidates staged during NUMA init.  Committed to the exclusive
 * pool by numa_commit_standby_nodes() once node_possible_map is finalized.
 */
static nodemask_t standby_candidates __initdata;

/**
 * numa_add_standby_node() - Stage a node as a standby pool candidate
 * @node: Node ID created as an empty standby node during NUMA init
 *
 * Records @node as a candidate for the exclusive pool.
 * Callers must also add @node to numa_nodes_parsed to mark it possible.
 */
void __init numa_add_standby_node(int node)
{
	node_set(node, standby_candidates);
}

/**
 * numa_commit_standby_nodes() - Publish staged standby nodes to the pool
 *
 * Registers the staged candidates that are present in node_possible_map
 * into the exclusive pool.  Restricting to possible nodes keeps the pool a
 * strict subset of node_possible_map, so a later claim can never return a
 * node that was dropped (e.g. by a fallback init or NUMA emulation).
 * Called once node_possible_map is final.
 */
void __init numa_commit_standby_nodes(void)
{
	nodes_and(exclusive_nodes, standby_candidates, node_possible_map);
}

/**
 * numa_request_exclusive_node() - Claim an available exclusive NUMA node
 *
 * Exclusive nodes are empty NUMA nodes registered at boot via the standby
 * node interfaces or standby= boot parameter.
 *
 * The caller takes exclusive ownership of the returned node and must
 * release it with numa_release_exclusive_node() when no longer needed.
 *
 * Return: a NUMA node ID on success, %NUMA_NO_NODE if none available.
 */
int numa_request_exclusive_node(void)
{
	int node;

	spin_lock(&exclusive_node_lock);
	node = first_node(exclusive_nodes);
	if (node < MAX_NUMNODES)
		node_clear(node, exclusive_nodes);
	else
		node = NUMA_NO_NODE;
	spin_unlock(&exclusive_node_lock);

	return node;
}
EXPORT_SYMBOL_GPL(numa_request_exclusive_node);

/**
 * numa_release_exclusive_node() - Release a previously claimed exclusive node
 * @node: Node ID previously returned by numa_request_exclusive_node()
 *
 * Returns the node to the exclusive pool.
 */
void numa_release_exclusive_node(int node)
{
	if (node == NUMA_NO_NODE)
		return;

	if (WARN_ON(node >= MAX_NUMNODES))
		return;

	spin_lock(&exclusive_node_lock);
	node_set(node, exclusive_nodes);
	spin_unlock(&exclusive_node_lock);
}
EXPORT_SYMBOL_GPL(numa_release_exclusive_node);
