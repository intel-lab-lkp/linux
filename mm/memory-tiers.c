// SPDX-License-Identifier: GPL-2.0
#include <linux/slab.h>
#include <linux/lockdep.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/memory.h>
#include <linux/memory-tiers.h>
#include <linux/notifier.h>
#include <linux/sched/sysctl.h>

#include "internal.h"

struct memory_tier {
	/* hierarchy of memory tiers */
	struct list_head list;
	/* list of all memory types part of this tier */
	struct list_head memory_types;
	/*
	 * start value of abstract distance. memory tier maps
	 * an abstract distance  range,
	 * adistance_start .. adistance_start + MEMTIER_CHUNK_SIZE
	 */
	int adistance_start;
	struct device dev;
	/* All the nodes that are part of all the lower memory tiers. */
	nodemask_t lower_tier_mask;
};

struct demotion_nodes {
	nodemask_t preferred;
};

struct node_memory_type_map {
	struct memory_dev_type *memtype;
	int map_count;
};

static DEFINE_MUTEX(memory_tier_lock);
static LIST_HEAD(memory_tiers);
/*
 * The list is used to store all memory types that are not created
 * by a device driver.
 */
static LIST_HEAD(default_memory_types);
static struct node_memory_type_map node_memory_types[MAX_NUMNODES];
struct memory_dev_type *default_dram_type;
nodemask_t default_dram_nodes __initdata = NODE_MASK_NONE;

static const struct bus_type memory_tier_subsys = {
	.name = "memory_tiering",
	.dev_name = "memory_tier",
};

#ifdef CONFIG_NUMA_BALANCING
/**
 * folio_use_access_time - check if a folio reuses cpupid for page access time
 * @folio: folio to check
 *
 * folio's _last_cpupid field is repurposed by memory tiering. In memory
 * tiering mode, cpupid of slow memory folio (not toptier memory) is used to
 * record page access time.
 *
 * Return: the folio _last_cpupid is used to record page access time
 */
bool folio_use_access_time(struct folio *folio)
{
	return (sysctl_numa_balancing_mode & NUMA_BALANCING_MEMORY_TIERING) &&
	       !node_is_toptier(folio_nid(folio));
}
#endif

#ifdef CONFIG_MIGRATION
static int top_tier_adistance;
/*
 * node_demotion[] examples:
 *
 * Example 1:
 *
 * Node 0 & 1 are CPU + DRAM nodes, node 2 & 3 are PMEM nodes.
 *
 * node distances:
 * node   0    1    2    3
 *    0  10   20   30   40
 *    1  20   10   40   30
 *    2  30   40   10   40
 *    3  40   30   40   10
 *
 * memory_tiers0 = 0-1
 * memory_tiers1 = 2-3
 *
 * node_demotion[0].preferred = 2
 * node_demotion[1].preferred = 3
 * node_demotion[2].preferred = <empty>
 * node_demotion[3].preferred = <empty>
 *
 * Example 2:
 *
 * Node 0 & 1 are CPU + DRAM nodes, node 2 is memory-only DRAM node.
 *
 * node distances:
 * node   0    1    2
 *    0  10   20   30
 *    1  20   10   30
 *    2  30   30   10
 *
 * memory_tiers0 = 0-2
 *
 * node_demotion[0].preferred = <empty>
 * node_demotion[1].preferred = <empty>
 * node_demotion[2].preferred = <empty>
 *
 * Example 3:
 *
 * Node 0 is CPU + DRAM nodes, Node 1 is HBM node, node 2 is PMEM node.
 *
 * node distances:
 * node   0    1    2
 *    0  10   20   30
 *    1  20   10   40
 *    2  30   40   10
 *
 * memory_tiers0 = 1
 * memory_tiers1 = 0
 * memory_tiers2 = 2
 *
 * node_demotion[0].preferred = 2
 * node_demotion[1].preferred = 0
 * node_demotion[2].preferred = <empty>
 *
 */
static struct demotion_nodes *node_demotion __read_mostly;
#endif /* CONFIG_MIGRATION */

static BLOCKING_NOTIFIER_HEAD(mt_adistance_algorithms);

/* The lock is used to protect `default_dram_perf*` info and nid. */
static DEFINE_MUTEX(default_dram_perf_lock);
static bool default_dram_perf_error;
static struct access_coordinate default_dram_perf;
static int default_dram_perf_ref_nid = NUMA_NO_NODE;
static const char *default_dram_perf_ref_source;

static inline struct memory_tier *to_memory_tier(struct device *device)
{
	return container_of(device, struct memory_tier, dev);
}

static __always_inline nodemask_t get_memtier_nodemask(struct memory_tier *memtier)
{
	nodemask_t nodes = NODE_MASK_NONE;
	struct memory_dev_type *memtype;

	list_for_each_entry(memtype, &memtier->memory_types, tier_sibling)
		nodes_or(nodes, nodes, memtype->nodes);

	return nodes;
}

static void memory_tier_device_release(struct device *dev)
{
	struct memory_tier *tier = to_memory_tier(dev);
	/*
	 * synchronize_rcu in clear_node_memory_tier makes sure
	 * we don't have rcu access to this memory tier.
	 */
	kfree(tier);
}

static ssize_t nodelist_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	int ret;
	nodemask_t nmask;

	mutex_lock(&memory_tier_lock);
	nmask = get_memtier_nodemask(to_memory_tier(dev));
	ret = sysfs_emit(buf, "%*pbl\n", nodemask_pr_args(&nmask));
	mutex_unlock(&memory_tier_lock);
	return ret;
}
static DEVICE_ATTR_RO(nodelist);

static struct attribute *memtier_dev_attrs[] = {
	&dev_attr_nodelist.attr,
	NULL
};

static const struct attribute_group memtier_dev_group = {
	.attrs = memtier_dev_attrs,
};

static const struct attribute_group *memtier_dev_groups[] = {
	&memtier_dev_group,
	NULL
};

static struct memory_tier *find_create_memory_tier(struct memory_dev_type *memtype)
{
	int ret;
	bool found_slot = false;
	struct memory_tier *memtier, *new_memtier;
	int adistance = memtype->adistance;
	unsigned int memtier_adistance_chunk_size = MEMTIER_CHUNK_SIZE;

	lockdep_assert_held_once(&memory_tier_lock);

	adistance = round_down(adistance, memtier_adistance_chunk_size);
	/*
	 * If the memtype is already part of a memory tier,
	 * just return that.
	 */
	if (!list_empty(&memtype->tier_sibling)) {
		list_for_each_entry(memtier, &memory_tiers, list) {
			if (adistance == memtier->adistance_start)
				return memtier;
		}
		WARN_ON(1);
		return ERR_PTR(-EINVAL);
	}

	list_for_each_entry(memtier, &memory_tiers, list) {
		if (adistance == memtier->adistance_start) {
			goto link_memtype;
		} else if (adistance < memtier->adistance_start) {
			found_slot = true;
			break;
		}
	}

	new_memtier = kzalloc(sizeof(struct memory_tier), GFP_KERNEL);
	if (!new_memtier)
		return ERR_PTR(-ENOMEM);

	new_memtier->adistance_start = adistance;
	INIT_LIST_HEAD(&new_memtier->list);
	INIT_LIST_HEAD(&new_memtier->memory_types);
	if (found_slot)
		list_add_tail(&new_memtier->list, &memtier->list);
	else
		list_add_tail(&new_memtier->list, &memory_tiers);

	new_memtier->dev.id = adistance >> MEMTIER_CHUNK_BITS;
	new_memtier->dev.bus = &memory_tier_subsys;
	new_memtier->dev.release = memory_tier_device_release;
	new_memtier->dev.groups = memtier_dev_groups;

	ret = device_register(&new_memtier->dev);
	if (ret) {
		list_del(&new_memtier->list);
		put_device(&new_memtier->dev);
		return ERR_PTR(ret);
	}
	memtier = new_memtier;

link_memtype:
	list_add(&memtype->tier_sibling, &memtier->memory_types);
	return memtier;
}

static struct memory_tier *__node_get_memory_tier(int node)
{
	pg_data_t *pgdat;

	pgdat = NODE_DATA(node);
	if (!pgdat)
		return NULL;
	/*
	 * Since we hold memory_tier_lock, we can avoid
	 * RCU read locks when accessing the details. No
	 * parallel updates are possible here.
	 */
	return rcu_dereference_check(pgdat->memtier,
				     lockdep_is_held(&memory_tier_lock));
}

#ifdef CONFIG_MIGRATION
bool node_is_toptier(int node)
{
	bool toptier;
	pg_data_t *pgdat;
	struct memory_tier *memtier;

	pgdat = NODE_DATA(node);
	if (!pgdat)
		return false;

	rcu_read_lock();
	memtier = rcu_dereference(pgdat->memtier);
	if (!memtier) {
		toptier = true;
		goto out;
	}
	if (memtier->adistance_start <= top_tier_adistance)
		toptier = true;
	else
		toptier = false;
out:
	rcu_read_unlock();
	return toptier;
}

void node_get_allowed_targets(pg_data_t *pgdat, nodemask_t *targets)
{
	struct memory_tier *memtier;

	/*
	 * pg_data_t.memtier updates includes a synchronize_rcu()
	 * which ensures that we either find NULL or a valid memtier
	 * in NODE_DATA. protect the access via rcu_read_lock();
	 */
	rcu_read_lock();
	memtier = rcu_dereference(pgdat->memtier);
	if (memtier)
		*targets = memtier->lower_tier_mask;
	else
		*targets = NODE_MASK_NONE;
	rcu_read_unlock();
}

/**
 * next_demotion_node() - Get the next node in the demotion path
 * @node: The starting node to lookup the next node
 *
 * Return: node id for next memory node in the demotion path hierarchy
 * from @node; NUMA_NO_NODE if @node is terminal.  This does not keep
 * @node online or guarantee that it *continues* to be the next demotion
 * target.
 */
int next_demotion_node(int node)
{
	struct demotion_nodes *nd;
	int target;

	if (!node_demotion)
		return NUMA_NO_NODE;

	nd = &node_demotion[node];

	/*
	 * node_demotion[] is updated without excluding this
	 * function from running.
	 *
	 * Make sure to use RCU over entire code blocks if
	 * node_demotion[] reads need to be consistent.
	 */
	rcu_read_lock();
	/*
	 * If there are multiple target nodes, just select one
	 * target node randomly.
	 *
	 * In addition, we can also use round-robin to select
	 * target node, but we should introduce another variable
	 * for node_demotion[] to record last selected target node,
	 * that may cause cache ping-pong due to the changing of
	 * last target node. Or introducing per-cpu data to avoid
	 * caching issue, which seems more complicated. So selecting
	 * target node randomly seems better until now.
	 */
	target = node_random(&nd->preferred);
	rcu_read_unlock();

	return target;
}

static void disable_all_demotion_targets(void)
{
	struct memory_tier *memtier;
	int node;

	for_each_node_state(node, N_MEMORY) {
		node_demotion[node].preferred = NODE_MASK_NONE;
		/*
		 * We are holding memory_tier_lock, it is safe
		 * to access pgda->memtier.
		 */
		memtier = __node_get_memory_tier(node);
		if (memtier)
			memtier->lower_tier_mask = NODE_MASK_NONE;
	}
	/*
	 * Ensure that the "disable" is visible across the system.
	 * Readers will see either a combination of before+disable
	 * state or disable+after.  They will never see before and
	 * after state together.
	 */
	synchronize_rcu();
}

static void dump_demotion_targets(void)
{
	int node;

	for_each_node_state(node, N_MEMORY) {
		struct memory_tier *memtier = __node_get_memory_tier(node);
		nodemask_t preferred = node_demotion[node].preferred;

		if (!memtier)
			continue;

		if (nodes_empty(preferred))
			pr_info("Demotion targets for Node %d: null\n", node);
		else
			pr_info("Demotion targets for Node %d: preferred: %*pbl, fallback: %*pbl\n",
				node, nodemask_pr_args(&preferred),
				nodemask_pr_args(&memtier->lower_tier_mask));
	}
}

/*
 * Find an automatic demotion target for all memory
 * nodes. Failing here is OK.  It might just indicate
 * being at the end of a chain.
 */
static void establish_demotion_targets(void)
{
	struct memory_tier *memtier;
	struct demotion_nodes *nd;
	int target = NUMA_NO_NODE, node;
	int distance, best_distance;
	nodemask_t tier_nodes, lower_tier;

	lockdep_assert_held_once(&memory_tier_lock);

	if (!node_demotion)
		return;

	disable_all_demotion_targets();

	for_each_node_state(node, N_MEMORY) {
		best_distance = -1;
		nd = &node_demotion[node];

		memtier = __node_get_memory_tier(node);
		if (!memtier || list_is_last(&memtier->list, &memory_tiers))
			continue;
		/*
		 * Get the lower memtier to find the  demotion node list.
		 */
		memtier = list_next_entry(memtier, list);
		tier_nodes = get_memtier_nodemask(memtier);
		/*
		 * find_next_best_node, use 'used' nodemask as a skip list.
		 * Add all memory nodes except the selected memory tier
		 * nodelist to skip list so that we find the best node from the
		 * memtier nodelist.
		 */
		nodes_andnot(tier_nodes, node_states[N_MEMORY], tier_nodes);

		/*
		 * Find all the nodes in the memory tier node list of same best distance.
		 * add them to the preferred mask. We randomly select between nodes
		 * in the preferred mask when allocating pages during demotion.
		 */
		do {
			target = find_next_best_node(node, &tier_nodes);
			if (target == NUMA_NO_NODE)
				break;

			distance = node_distance(node, target);
			if (distance == best_distance || best_distance == -1) {
				best_distance = distance;
				node_set(target, nd->preferred);
			} else {
				break;
			}
		} while (1);
	}
	/*
	 * Promotion is allowed from a memory tier to higher
	 * memory tier only if the memory tier doesn't include
	 * compute. We want to skip promotion from a memory tier,
	 * if any node that is part of the memory tier have CPUs.
	 * Once we detect such a memory tier, we consider that tier
	 * as top tiper from which promotion is not allowed.
	 */
	list_for_each_entry_reverse(memtier, &memory_tiers, list) {
		tier_nodes = get_memtier_nodemask(memtier);
		nodes_and(tier_nodes, node_states[N_CPU], tier_nodes);
		if (!nodes_empty(tier_nodes)) {
			/*
			 * abstract distance below the max value of this memtier
			 * is considered toptier.
			 */
			top_tier_adistance = memtier->adistance_start +
						MEMTIER_CHUNK_SIZE - 1;
			break;
		}
	}
	/*
	 * Now build the lower_tier mask for each node collecting node mask from
	 * all memory tier below it. This allows us to fallback demotion page
	 * allocation to a set of nodes that is closer the above selected
	 * preferred node.
	 */
	lower_tier = node_states[N_MEMORY];
	list_for_each_entry(memtier, &memory_tiers, list) {
		/*
		 * Keep removing current tier from lower_tier nodes,
		 * This will remove all nodes in current and above
		 * memory tier from the lower_tier mask.
		 */
		tier_nodes = get_memtier_nodemask(memtier);
		nodes_andnot(lower_tier, lower_tier, tier_nodes);
		memtier->lower_tier_mask = lower_tier;
	}

	dump_demotion_targets();
}

#else
static inline void establish_demotion_targets(void) {}
#endif /* CONFIG_MIGRATION */

static inline void __init_node_memory_type(int node, struct memory_dev_type *memtype)
{
	if (!node_memory_types[node].memtype)
		node_memory_types[node].memtype = memtype;
	/*
	 * for each device getting added in the same NUMA node
	 * with this specific memtype, bump the map count. We
	 * Only take memtype device reference once, so that
	 * changing a node memtype can be done by dropping the
	 * only reference count taken here.
	 */

	if (node_memory_types[node].memtype == memtype) {
		if (!node_memory_types[node].map_count++)
			kref_get(&memtype->kref);
	}
}

static struct memory_tier *set_node_memory_tier(int node)
{
	struct memory_tier *memtier;
	struct memory_dev_type *memtype = default_dram_type;
	int adist = MEMTIER_ADISTANCE_DRAM;
	pg_data_t *pgdat = NODE_DATA(node);


	lockdep_assert_held_once(&memory_tier_lock);

	if (!node_state(node, N_MEMORY))
		return ERR_PTR(-EINVAL);

	mt_calc_adistance(node, &adist);
	if (!node_memory_types[node].memtype) {
		memtype = mt_find_alloc_memory_type(adist, &default_memory_types);
		if (IS_ERR(memtype)) {
			memtype = default_dram_type;
			pr_info("Failed to allocate a memory type. Fall back.\n");
		}
	}

	__init_node_memory_type(node, memtype);

	memtype = node_memory_types[node].memtype;
	node_set(node, memtype->nodes);
	memtier = find_create_memory_tier(memtype);
	if (!IS_ERR(memtier))
		rcu_assign_pointer(pgdat->memtier, memtier);
	return memtier;
}

static void destroy_memory_tier(struct memory_tier *memtier)
{
	list_del(&memtier->list);
	device_unregister(&memtier->dev);
}

static bool clear_node_memory_tier(int node)
{
	bool cleared = false;
	pg_data_t *pgdat;
	struct memory_tier *memtier;

	pgdat = NODE_DATA(node);
	if (!pgdat)
		return false;

	/*
	 * Make sure that anybody looking at NODE_DATA who finds
	 * a valid memtier finds memory_dev_types with nodes still
	 * linked to the memtier. We achieve this by waiting for
	 * rcu read section to finish using synchronize_rcu.
	 * This also enables us to free the destroyed memory tier
	 * with kfree instead of kfree_rcu
	 */
	memtier = __node_get_memory_tier(node);
	if (memtier) {
		struct memory_dev_type *memtype;

		rcu_assign_pointer(pgdat->memtier, NULL);
		synchronize_rcu();
		memtype = node_memory_types[node].memtype;
		node_clear(node, memtype->nodes);
		if (nodes_empty(memtype->nodes)) {
			list_del_init(&memtype->tier_sibling);
			if (list_empty(&memtier->memory_types))
				destroy_memory_tier(memtier);
		}
		cleared = true;
	}
	return cleared;
}

static void release_memtype(struct kref *kref)
{
	struct memory_dev_type *memtype;

	memtype = container_of(kref, struct memory_dev_type, kref);
	kfree(memtype);
}

struct memory_dev_type *alloc_memory_type(int adistance)
{
	struct memory_dev_type *memtype;

	memtype = kmalloc(sizeof(*memtype), GFP_KERNEL);
	if (!memtype)
		return ERR_PTR(-ENOMEM);

	memtype->adistance = adistance;
	INIT_LIST_HEAD(&memtype->tier_sibling);
	memtype->nodes  = NODE_MASK_NONE;
	kref_init(&memtype->kref);
	return memtype;
}
EXPORT_SYMBOL_GPL(alloc_memory_type);

void put_memory_type(struct memory_dev_type *memtype)
{
	kref_put(&memtype->kref, release_memtype);
}
EXPORT_SYMBOL_GPL(put_memory_type);

void init_node_memory_type(int node, struct memory_dev_type *memtype)
{

	mutex_lock(&memory_tier_lock);
	__init_node_memory_type(node, memtype);
	mutex_unlock(&memory_tier_lock);
}
EXPORT_SYMBOL_GPL(init_node_memory_type);

void clear_node_memory_type(int node, struct memory_dev_type *memtype)
{
	mutex_lock(&memory_tier_lock);
	if (node_memory_types[node].memtype == memtype || !memtype)
		node_memory_types[node].map_count--;
	/*
	 * If we umapped all the attached devices to this node,
	 * clear the node memory type.
	 */
	if (!node_memory_types[node].map_count) {
		memtype = node_memory_types[node].memtype;
		node_memory_types[node].memtype = NULL;
		put_memory_type(memtype);
	}
	mutex_unlock(&memory_tier_lock);
}
EXPORT_SYMBOL_GPL(clear_node_memory_type);

struct memory_dev_type *mt_find_alloc_memory_type(int adist, struct list_head *memory_types)
{
	struct memory_dev_type *mtype;

	list_for_each_entry(mtype, memory_types, list)
		if (mtype->adistance == adist)
			return mtype;

	mtype = alloc_memory_type(adist);
	if (IS_ERR(mtype))
		return mtype;

	list_add(&mtype->list, memory_types);

	return mtype;
}
EXPORT_SYMBOL_GPL(mt_find_alloc_memory_type);

void mt_put_memory_types(struct list_head *memory_types)
{
	struct memory_dev_type *mtype, *mtn;

	list_for_each_entry_safe(mtype, mtn, memory_types, list) {
		list_del(&mtype->list);
		put_memory_type(mtype);
	}
}
EXPORT_SYMBOL_GPL(mt_put_memory_types);

/*
 * This is invoked via `late_initcall()` to initialize memory tiers for
 * memory nodes, both with and without CPUs. After the initialization of
 * firmware and devices, adistance algorithms are expected to be provided.
 */
static int __init memory_tier_late_init(void)
{
	int nid;
	struct memory_tier *memtier;

	get_online_mems();
	guard(mutex)(&memory_tier_lock);

	/* Assign each uninitialized N_MEMORY node to a memory tier. */
	for_each_node_state(nid, N_MEMORY) {
		/*
		 * Some device drivers may have initialized
		 * memory tiers, potentially bringing memory nodes
		 * online and configuring memory tiers.
		 * Exclude them here.
		 */
		if (node_memory_types[nid].memtype)
			continue;

		memtier = set_node_memory_tier(nid);
		if (IS_ERR(memtier))
			continue;
	}

	establish_demotion_targets();
	put_online_mems();

	return 0;
}
late_initcall(memory_tier_late_init);

static void dump_hmem_attrs(struct access_coordinate *coord, const char *prefix)
{
	pr_info(
"%sread_latency: %u, write_latency: %u, read_bandwidth: %u, write_bandwidth: %u\n",
		prefix, coord->read_latency, coord->write_latency,
		coord->read_bandwidth, coord->write_bandwidth);
}

int mt_set_default_dram_perf(int nid, struct access_coordinate *perf,
			     const char *source)
{
	guard(mutex)(&default_dram_perf_lock);
	if (default_dram_perf_error)
		return -EIO;

	if (perf->read_latency + perf->write_latency == 0 ||
	    perf->read_bandwidth + perf->write_bandwidth == 0)
		return -EINVAL;

	if (default_dram_perf_ref_nid == NUMA_NO_NODE) {
		default_dram_perf = *perf;
		default_dram_perf_ref_nid = nid;
		default_dram_perf_ref_source = kstrdup(source, GFP_KERNEL);
		return 0;
	}

	/*
	 * The performance of all default DRAM nodes is expected to be
	 * same (that is, the variation is less than 10%).  And it
	 * will be used as base to calculate the abstract distance of
	 * other memory nodes.
	 */
	if (abs(perf->read_latency - default_dram_perf.read_latency) * 10 >
	    default_dram_perf.read_latency ||
	    abs(perf->write_latency - default_dram_perf.write_latency) * 10 >
	    default_dram_perf.write_latency ||
	    abs(perf->read_bandwidth - default_dram_perf.read_bandwidth) * 10 >
	    default_dram_perf.read_bandwidth ||
	    abs(perf->write_bandwidth - default_dram_perf.write_bandwidth) * 10 >
	    default_dram_perf.write_bandwidth) {
		pr_info(
"memory-tiers: the performance of DRAM node %d mismatches that of the reference\n"
"DRAM node %d.\n", nid, default_dram_perf_ref_nid);
		pr_info("  performance of reference DRAM node %d from %s:\n",
			default_dram_perf_ref_nid, default_dram_perf_ref_source);
		dump_hmem_attrs(&default_dram_perf, "    ");
		pr_info("  performance of DRAM node %d from %s:\n", nid, source);
		dump_hmem_attrs(perf, "    ");
		pr_info(
"  disable default DRAM node performance based abstract distance algorithm.\n");
		default_dram_perf_error = true;
		return -EINVAL;
	}

	return 0;
}

int mt_perf_to_adistance(struct access_coordinate *perf, int *adist)
{
	guard(mutex)(&default_dram_perf_lock);
	if (default_dram_perf_error)
		return -EIO;

	if (perf->read_latency + perf->write_latency == 0 ||
	    perf->read_bandwidth + perf->write_bandwidth == 0)
		return -EINVAL;

	if (default_dram_perf_ref_nid == NUMA_NO_NODE)
		return -ENOENT;

	/*
	 * The abstract distance of a memory node is in direct proportion to
	 * its memory latency (read + write) and inversely proportional to its
	 * memory bandwidth (read + write).  The abstract distance, memory
	 * latency, and memory bandwidth of the default DRAM nodes are used as
	 * the base.
	 */
	*adist = MEMTIER_ADISTANCE_DRAM *
		(perf->read_latency + perf->write_latency) /
		(default_dram_perf.read_latency + default_dram_perf.write_latency) *
		(default_dram_perf.read_bandwidth + default_dram_perf.write_bandwidth) /
		(perf->read_bandwidth + perf->write_bandwidth);

	return 0;
}
EXPORT_SYMBOL_GPL(mt_perf_to_adistance);

/**
 * register_mt_adistance_algorithm() - Register memory tiering abstract distance algorithm
 * @nb: The notifier block which describe the algorithm
 *
 * Return: 0 on success, errno on error.
 *
 * Every memory tiering abstract distance algorithm provider needs to
 * register the algorithm with register_mt_adistance_algorithm().  To
 * calculate the abstract distance for a specified memory node, the
 * notifier function will be called unless some high priority
 * algorithm has provided result.  The prototype of the notifier
 * function is as follows,
 *
 *   int (*algorithm_notifier)(struct notifier_block *nb,
 *                             unsigned long nid, void *data);
 *
 * Where "nid" specifies the memory node, "data" is the pointer to the
 * returned abstract distance (that is, "int *adist").  If the
 * algorithm provides the result, NOTIFY_STOP should be returned.
 * Otherwise, return_value & %NOTIFY_STOP_MASK == 0 to allow the next
 * algorithm in the chain to provide the result.
 */
int register_mt_adistance_algorithm(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&mt_adistance_algorithms, nb);
}
EXPORT_SYMBOL_GPL(register_mt_adistance_algorithm);

/**
 * unregister_mt_adistance_algorithm() - Unregister memory tiering abstract distance algorithm
 * @nb: the notifier block which describe the algorithm
 *
 * Return: 0 on success, errno on error.
 */
int unregister_mt_adistance_algorithm(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&mt_adistance_algorithms, nb);
}
EXPORT_SYMBOL_GPL(unregister_mt_adistance_algorithm);

/**
 * mt_calc_adistance() - Calculate abstract distance with registered algorithms
 * @node: the node to calculate abstract distance for
 * @adist: the returned abstract distance
 *
 * Return: if return_value & %NOTIFY_STOP_MASK != 0, then some
 * abstract distance algorithm provides the result, and return it via
 * @adist.  Otherwise, no algorithm can provide the result and @adist
 * will be kept as it is.
 */
int mt_calc_adistance(int node, int *adist)
{
	return blocking_notifier_call_chain(&mt_adistance_algorithms, node, adist);
}
EXPORT_SYMBOL_GPL(mt_calc_adistance);

static int __meminit memtier_hotplug_callback(struct notifier_block *self,
					      unsigned long action, void *_arg)
{
	struct memory_tier *memtier;
	struct node_notify *nn = _arg;

	switch (action) {
	case NODE_REMOVED_LAST_MEMORY:
		mutex_lock(&memory_tier_lock);
		if (clear_node_memory_tier(nn->nid))
			establish_demotion_targets();
		mutex_unlock(&memory_tier_lock);
		break;
	case NODE_ADDED_FIRST_MEMORY:
		mutex_lock(&memory_tier_lock);
		memtier = set_node_memory_tier(nn->nid);
		if (!IS_ERR(memtier))
			establish_demotion_targets();
		mutex_unlock(&memory_tier_lock);
		break;
	}

	return notifier_from_errno(0);
}

static int __init memory_tier_init(void)
{
	int ret;

	ret = subsys_virtual_register(&memory_tier_subsys, NULL);
	if (ret)
		panic("%s() failed to register memory tier subsystem\n", __func__);

#ifdef CONFIG_MIGRATION
	node_demotion = kcalloc(nr_node_ids, sizeof(struct demotion_nodes),
				GFP_KERNEL);
	WARN_ON(!node_demotion);
#endif

	mutex_lock(&memory_tier_lock);
	/*
	 * For now we can have 4 faster memory tiers with smaller adistance
	 * than default DRAM tier.
	 */
	default_dram_type = mt_find_alloc_memory_type(MEMTIER_ADISTANCE_DRAM,
						      &default_memory_types);
	mutex_unlock(&memory_tier_lock);
	if (IS_ERR(default_dram_type))
		panic("%s() failed to allocate default DRAM tier\n", __func__);

	/* Record nodes with memory and CPU to set default DRAM performance. */
	nodes_and(default_dram_nodes, node_states[N_MEMORY],
		  node_states[N_CPU]);

	hotplug_node_notifier(memtier_hotplug_callback, MEMTIER_HOTPLUG_PRI);
	return 0;
}
subsys_initcall(memory_tier_init);

bool numa_demotion_enabled = false;

#ifdef CONFIG_MIGRATION
#ifdef CONFIG_SYSFS
static ssize_t demotion_enabled_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", str_true_false(numa_demotion_enabled));
}

static ssize_t demotion_enabled_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	ssize_t ret;
	bool before = numa_demotion_enabled;

	ret = kstrtobool(buf, &numa_demotion_enabled);
	if (ret)
		return ret;

	/*
	 * Reset kswapd_failures statistics. They may no longer be
	 * valid since the policy for kswapd has changed.
	 */
	if (before == false && numa_demotion_enabled == true) {
		struct pglist_data *pgdat;

		for_each_online_pgdat(pgdat)
			atomic_set(&pgdat->kswapd_failures, 0);
	}

	return count;
}

static struct kobj_attribute numa_demotion_enabled_attr =
	__ATTR_RW(demotion_enabled);

static struct attribute *numa_attrs[] = {
	&numa_demotion_enabled_attr.attr,
	NULL,
};

static const struct attribute_group numa_attr_group = {
	.attrs = numa_attrs,
};

static int __init numa_init_sysfs(void)
{
	int err;
	struct kobject *numa_kobj;

	numa_kobj = kobject_create_and_add("numa", mm_kobj);
	if (!numa_kobj) {
		pr_err("failed to create numa kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(numa_kobj, &numa_attr_group);
	if (err) {
		pr_err("failed to register numa group\n");
		goto delete_obj;
	}
	return 0;

delete_obj:
	kobject_put(numa_kobj);
	return err;
}
subsys_initcall(numa_init_sysfs);
#endif /* CONFIG_SYSFS */
#endif

/**
 * enum mp_nodes_type - Selector for which subset of a package to return
 * @MP_NODES_ALL:       All NUMA nodes that belong to the package.
 * @MP_NODES_CPU:       Only CPU nodes in the package.
 * @MP_NODES_MEM_ONLY:  Only memory-only nodes (e.g. CXL/HBM) in the package.
 *
 * Used internally to choose which nodemask to expose for a given package.
 */
enum mp_nodes_type {
	MP_NODES_ALL,
	MP_NODES_CPU,
	MP_NODES_MEM_ONLY
};

/**
 * struct memory_package - Per-socket (physical package) container
 * @package_id:          Physical socket/package id (from topology).
 * @nodes:               Nodemask of all member nodes in this package.
 * @cpu_nodes:           Nodemask of CPU nodes in this package.
 * @memory_only_nodes:   Nodemask of memory-only nodes in this package.
 * @cpu_list:            List head of CPU-type members.
 * @memory_only_list:    List head of memory-only members.
 * @list:                Linkage on the global @memory_packages list.
 *
 * A memory_package groups NUMA nodes that share the same physical CPU package.
 * The masks are used to implement socket-local placement/demotion/promotion.
 */
struct memory_package {
	int package_id;
	nodemask_t nodes;
	nodemask_t cpu_nodes;
	nodemask_t memory_only_nodes;
	struct list_head cpu_list;
	struct list_head memory_only_list;
	struct list_head list;
};

/**
 * enum mpn_source_flags - Source used to resolve a node's package membership
 * @MPN_SRC_UNKNOWN:     Unknown/unspecified.
 * @MPN_SRC_CPU:         Directly resolved from a CPU node (1:1).
 * @MPN_SRC_INITIATOR:   Resolved via an initiator CPU node provided by a driver.
 * @MPN_SRC_SLIT:        Resolved via SLIT/nearest-node.
 *
 * These flags are informational; they describe how a given node was bound to
 * its package and help with policy decisions later.
 */
enum mpn_source_flags {
	MPN_SRC_UNKNOWN		= 0,
	MPN_SRC_CPU		= BIT(1),
	MPN_SRC_INITIATOR	= BIT(2),
	MPN_SRC_SLIT		= BIT(3)
};

/**
 * struct memory_package_node - Per-node membership and preferences
 * @nid:              NUMA node id for this entry.
 * @initiator_nid:    CPU nid that served as the initiator when resolving @nid.
 * @package_id:       Resolved package id that @nid belongs to.
 * @source_flags:     One of &enum mpn_source_flags describing the resolution.
 * @preferred:        Opposite-type nearest candidates inside the same package.
 * @package:          Pointer to the owning &struct memory_package (NULL until bound).
 * @package_entry:    Linkage on the owning package's type list.
 *
 * Each NUMA node that participates in socket-aware policy gets a wrapper entry
 * that caches package membership and the precomputed set of preferred targets.
 */
struct memory_package_node {
	int nid;
	int initiator_nid;
	int package_id;
	int source_flags;
	nodemask_t preferred;
	struct memory_package *package;
	struct list_head package_entry;
};

#define node_is_memory_only(_nid) \
	(node_state((_nid), N_MEMORY) && !node_state((_nid), N_CPU))

static BLOCKING_NOTIFIER_HEAD(mp_package_algorithms);

static LIST_HEAD(memory_packages);
static struct memory_package_node *mpns[MAX_NUMNODES];
static DEFINE_MUTEX(memory_package_lock);

/**
 * register_mp_package_notifier - Register a package resolution algorithm
 * @notifier: Notifier called with the nid to resolve (see mp_probe_package_id()).
 *
 * Drivers (e.g., CXL region/decoder code) register here to supply a package
 * hint for newly appearing nodes. The notifier is invoked during nid->package
 * resolution.
 *
 * Return: 0 on success, negative errno on failure.
 */
int register_mp_package_notifier(struct notifier_block *notifier)
{
	return blocking_notifier_chain_register(&mp_package_algorithms, notifier);
}
EXPORT_SYMBOL_GPL(register_mp_package_notifier);

/**
 * unregister_mp_package_notifier - Unregister a package resolution algorithm
 * @notifier: Notifier previously registered with register_mp_package_notifier().
 */
void unregister_mp_package_notifier(struct notifier_block *notifier)
{
	blocking_notifier_chain_unregister(&mp_package_algorithms, notifier);
}
EXPORT_SYMBOL_GPL(unregister_mp_package_notifier);

/**
 * mp_probe_package_id - Invoke registered notifiers to resolve a node's package
 * @nid: NUMA node id to resolve.
 *
 * Calls the blocking notifier chain to let subsystems provide an initiator or
 * package id for @nid.
 *
 * Return: Notifier return code (>=0 typically); negative errno on failure.
 */
int mp_probe_package_id(int nid)
{
	return blocking_notifier_call_chain(&mp_package_algorithms, nid, NULL);
}
EXPORT_SYMBOL_GPL(mp_probe_package_id);

static int mp_node_to_package_id(int nid)
{
	int package_id = -EINVAL;
	unsigned int first_cpu;
	const struct cpumask *cpu_mask;

	if (!node_state(nid, N_CPU))
		goto out;

	cpu_mask = cpumask_of_node(nid);
	if (cpumask_empty(cpu_mask)) {
		pr_err("node%d: CPU mask is empty\n", nid);
		goto out;
	}

	first_cpu = cpumask_first(cpu_mask);
	if (first_cpu >= nr_cpu_ids) {
		pr_err("node%d: CPU (%d) out of range\n", nid, first_cpu);
		goto out;
	}

	/*
	 * Map the first CPU in this node’s cpumask to its physical package id.
	 * This ties the NUMA node to a socket (package) using topology info.
	 */
	package_id = topology_physical_package_id(first_cpu);
	if (package_id < 0) {
		pr_err("node%d: failed to resolve package id (%d)\n", nid, package_id);
		package_id = -EINVAL;
		goto out;
	}

out:
	return package_id;
}

static void update_package_preferred(struct memory_package *mp)
{
	struct memory_package_node *mpn;

	lockdep_assert_held(&memory_package_lock);

	/*
	 * For each CPU node, compute its preferred set as the nearest
	 * memory-only node(s) within the same package. If the package has
	 * no memory-only nodes, fall back to a self-reference so callers
	 * never see an empty preferred set.
	 */
	list_for_each_entry(mpn, &mp->cpu_list, package_entry) {
		nodes_clear(mpn->preferred);
		if (!nodes_empty(mp->memory_only_nodes))
			nearest_nodes_nodemask(mpn->nid, &mp->memory_only_nodes,
					       &mpn->preferred);
		else
			node_set(mpn->nid, mpn->preferred);
	}

	/*
	 * Symmetrically, for each memory-only node, compute its preferred set
	 * as the nearest CPU node(s) within the same package. If the package
	 * has no CPU nodes, fall back to a self-reference.
	 */
	list_for_each_entry(mpn, &mp->memory_only_list, package_entry) {
		nodes_clear(mpn->preferred);
		if (!nodes_empty(mp->cpu_nodes))
			nearest_nodes_nodemask(mpn->nid, &mp->cpu_nodes,
					       &mpn->preferred);
		else
			node_set(mpn->nid, mpn->preferred);
	}
}

static inline bool memory_package_is_empty(struct memory_package *mp)
{
	lockdep_assert_held(&memory_package_lock);

	return (nodes_empty(mp->cpu_nodes) && nodes_empty(mp->memory_only_nodes));
}

static inline bool package_node_is_valid(int nid)
{
	if (!mpns[nid]) {
		pr_err("mpns[%d] is NULL\n", nid);
		return false;
	}

	if (nodes_empty(mpns[nid]->preferred) || (mpns[nid]->package == NULL)) {
		pr_err("nid %d: package or preferred mask not initialized\n", nid);
		return false;
	}

	return true;
}

static struct memory_package *create_memory_package(int package_id)
{
	struct memory_package *mempackage;

	mempackage = kzalloc(sizeof(*mempackage), GFP_KERNEL);
	if (!mempackage)
		return ERR_PTR(-ENOMEM);

	mempackage->package_id = package_id;
	mempackage->nodes = NODE_MASK_NONE;
	mempackage->cpu_nodes = NODE_MASK_NONE;
	mempackage->memory_only_nodes = NODE_MASK_NONE;
	INIT_LIST_HEAD(&mempackage->cpu_list);
	INIT_LIST_HEAD(&mempackage->memory_only_list);
	INIT_LIST_HEAD(&mempackage->list);

	return mempackage;
}

static void destroy_memory_package(struct memory_package *mp)
{
	lockdep_assert_held(&memory_package_lock);

	if (memory_package_is_empty(mp)) {
		list_del(&mp->list);
		kfree(mp);
	}
}

static struct memory_package *find_create_memory_package(int package_id)
{
	struct memory_package *mempackage;

	mutex_lock(&memory_package_lock);
	list_for_each_entry(mempackage, &memory_packages, list) {
		/*
		 * If a package for this package_id already exists, reuse it
		 * instead of allocating a new one.
		 */
		if (mempackage->package_id == package_id) {
			mutex_unlock(&memory_package_lock);
			return mempackage;
		}
	}
	mutex_unlock(&memory_package_lock);

	mempackage = create_memory_package(package_id);
	if (IS_ERR(mempackage))
		return ERR_PTR(-ENOMEM);

	mutex_lock(&memory_package_lock);
	list_add(&mempackage->list, &memory_packages);
	mutex_unlock(&memory_package_lock);

	return mempackage;
}

static int bind_node_to_package(int nid)
{
	int ret = 0, package_id;
	struct memory_package *mp;

	mutex_lock(&memory_package_lock);
	if (!mpns[nid]) {
		ret = -EINVAL;
		goto unlock_out;
	}
	package_id = mpns[nid]->package_id;
	mutex_unlock(&memory_package_lock);

	mp = find_create_memory_package(package_id);
	if (IS_ERR(mp)) {
		ret = PTR_ERR(mp);
		goto out;
	}

	mutex_lock(&memory_package_lock);
	mpns[nid]->package = mp;
	node_set(mpns[nid]->nid, mp->nodes);
	if (node_is_memory_only(mpns[nid]->nid)) {
		node_set(mpns[nid]->nid, mp->memory_only_nodes);
		list_add(&mpns[nid]->package_entry, &mp->memory_only_list);
	} else {
		node_set(mpns[nid]->nid, mp->cpu_nodes);
		list_add(&mpns[nid]->package_entry, &mp->cpu_list);
	}
	update_package_preferred(mp);

unlock_out:
	mutex_unlock(&memory_package_lock);
out:
	pr_info("memory_package %d: nodes=%*pbl cpu=%*pbl memery_only=%*pbl\n",
		mp->package_id,
		nodemask_pr_args(&mp->nodes),
		nodemask_pr_args(&mp->cpu_nodes),
		nodemask_pr_args(&mp->memory_only_nodes));

	return ret;
}

static void unbind_node_to_package(struct memory_package *mp, int nid)
{
	lockdep_assert_held(&memory_package_lock);

	node_clear(nid, mp->nodes);
	if (node_state(nid, N_CPU))
		node_clear(nid, mp->cpu_nodes);
	else
		node_clear(nid, mp->memory_only_nodes);

	if (mpns[nid])
		list_del(&mpns[nid]->package_entry);

	update_package_preferred(mp);
}

static struct memory_package_node *create_package_node(int nid, int initiator_nid)
{
	int cpu_nid, package_id;
	int source_flags;
	struct memory_package_node *mpn;

	if (node_state(nid, N_CPU)) {
		cpu_nid = nid;
		source_flags = MPN_SRC_CPU;
	} else {
		if (initiator_nid >= 0) {
			cpu_nid = initiator_nid;
			source_flags = MPN_SRC_INITIATOR;
		} else {
			/*
			 * No driver-supplied initiator: fall back to the
			 * nearest CPU node (via SLIT/numa_distance).
			 */
			cpu_nid = numa_nearest_node(nid, N_CPU);
			source_flags = MPN_SRC_SLIT;
		}
	}

	package_id = mp_node_to_package_id(cpu_nid);
	if (package_id < 0)
		return ERR_PTR(-EINVAL);

	mpn = kzalloc(sizeof(*mpn), GFP_KERNEL);
	if (!mpn)
		return ERR_PTR(-ENOMEM);

	mpn->nid = nid;
	mpn->initiator_nid = cpu_nid;
	mpn->package_id = package_id;
	mpn->source_flags = source_flags;
	mpn->preferred = NODE_MASK_NONE;
	mpn->package = NULL;
	INIT_LIST_HEAD(&mpn->package_entry);

	return mpn;
}

static void __destroy_package_node(int nid)
{
	struct memory_package_node *mpn;
	struct memory_package *mp;

	lockdep_assert_held(&memory_package_lock);

	mpn = mpns[nid];
	if (!mpn)
		return;

	mp = mpn->package;
	if (mp) {
		unbind_node_to_package(mp, nid);
		mpn->package = NULL;

		if (memory_package_is_empty(mp))
			destroy_memory_package(mp);
	}

	mpns[nid] = NULL;
	kfree(mpn);
}

static void destroy_package_node(int nid)
{
	mutex_lock(&memory_package_lock);
	__destroy_package_node(nid);
	mutex_unlock(&memory_package_lock);
}

static int find_package_node(int nid, int initiator_nid)
{
	int mpn_nid = NUMA_NO_NODE;

	mutex_lock(&memory_package_lock);
	if (mpns[nid]) {
		/*
		 * SLIT-derived entries are provisional; if a driver later
		 * provides an explicit initiator, drop the provisional
		 * entry and rebuild with the stronger hint.
		 */
		if (mpns[nid]->source_flags == MPN_SRC_SLIT && initiator_nid >= 0)
			__destroy_package_node(nid);
		else
			mpn_nid = nid;
	}
	mutex_unlock(&memory_package_lock);

	return mpn_nid;
}

static int find_create_package_node(int nid, int initiator_nid)
{
	int mpn_nid;
	struct memory_package_node *mpn;

	mpn_nid = find_package_node(nid, initiator_nid);
	if (mpn_nid != NUMA_NO_NODE)
		return mpn_nid;

	mpn = create_package_node(nid, initiator_nid);
	if (IS_ERR(mpn))
		return PTR_ERR(mpn);

	mutex_lock(&memory_package_lock);
	mpns[nid] = mpn;
	mutex_unlock(&memory_package_lock);

	return nid;
}

static int create_node_with_package(int nid)
{
	int ret;

	ret = find_create_package_node(nid, NUMA_NO_NODE);
	if (ret < 0) {
		pr_err("package_node(%d) failed: %d\n", nid, ret);
		return ret;
	}

	ret = bind_node_to_package(nid);
	if (ret) {
		pr_err("bind_node_to_package(%d) failed: %d\n", nid, ret);
		return ret;
	}

	return 0;
}

/**
 * mp_add_package_node_by_initiator - Add a node with an initiator
 * @nid:            Target NUMA node to add.
 * @initiator_nid:  CPU nid used to resolve @nid's package (>=0).
 *
 * Ensures that a &struct memory_package_node exists for @nid and that its
 * package_id is determined using @initiator_nid when provided. Binding to the
 * package is not performed here.
 *
 * Return: 0 on success; negative errno on failure.
 */
int mp_add_package_node_by_initiator(int nid, int initiator_nid)
{
	int ret;

	ret = find_create_package_node(nid, initiator_nid);
	if (ret < 0) {
		pr_err("find_create_package_node(nid=%d, initiator=%d) failed: %d\n",
		       nid, initiator_nid, ret);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mp_add_package_node_by_initiator);

/**
 * mp_add_package_node - Add a node, resolving package automatically
 * @nid: Target NUMA node to add.
 *
 * Wrapper over mp_add_package_node_by_initiator() that requests automatic
 * initiator resolution (e.g., nearest CPU).
 *
 * Return: 0 on success; negative errno on failure.
 */
int mp_add_package_node(int nid)
{
	return mp_add_package_node_by_initiator(nid, NUMA_NO_NODE);
}
EXPORT_SYMBOL_GPL(mp_add_package_node);

static int __mp_get_preferred_nodemask(int nid, enum mp_nodes_type node_type,
				    nodemask_t *out)
{
	int ret = 0;

	if (!out) {
		ret = -EINVAL;
		goto out;
	}

	nodes_clear(*out);

	if (nid < 0 || nid >= MAX_NUMNODES) {
		ret = -EINVAL;
		goto out;
	}

	if (node_type == MP_NODES_CPU) {
		if (node_is_memory_only(nid)) {
			pr_err("nid %d is a memory-only node\n", nid);
			ret = -EINVAL;
			goto out;
		}
	} else if (node_type == MP_NODES_MEM_ONLY) {
		if (!node_is_memory_only(nid)) {
			pr_err("nid %d is a CPU node\n", nid);
			ret = -EINVAL;
			goto out;
		}
	} else {
		pr_err("invalid node type: %d\n", (int)node_type);
		ret = -EINVAL;
		goto out;
	}

	if (!package_node_is_valid(nid)) {
		ret = -ENOENT;
		goto out;
	}

	nodes_copy(*out, mpns[nid]->preferred);

out:
	return ret;
}

static int __mp_get_package_nodemask(int nid, enum mp_nodes_type node_type,
				     nodemask_t *out)
{
	int ret = 0;

	if (!out) {
		ret = -EINVAL;
		goto out;
	}

	nodes_clear(*out);

	if (nid < 0 || nid >= MAX_NUMNODES) {
		ret = -EINVAL;
		goto out;
	}

	if (!package_node_is_valid(nid)) {
		ret = -ENOENT;
		goto out;
	}

	switch (node_type) {
	case MP_NODES_ALL:
		nodes_copy(*out, mpns[nid]->package->nodes);
		break;
	case MP_NODES_CPU:
		nodes_copy(*out, mpns[nid]->package->cpu_nodes);
		break;
	case MP_NODES_MEM_ONLY:
		nodes_copy(*out, mpns[nid]->package->memory_only_nodes);
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

out:
	return ret;
}

#if CONFIG_MIGRATION
/**
 * mp_next_demotion_nodemask - Demotion candidates within a package
 * @nid: CPU node from which memory would be demoted.
 * @out: Output nodemask of nearest memory-only targets in the same package.
 *
 * Return: 0 on success; negative errno if @nid is invalid or not initialized.
 */
int mp_next_demotion_nodemask(int nid, nodemask_t *out)
{
	return __mp_get_preferred_nodemask(nid, MP_NODES_CPU, out);
}
EXPORT_SYMBOL_GPL(mp_next_demotion_nodemask);

/**
 * mp_next_demotion_node - Pick one demotion target
 * @nid: CPU node from which memory would be demoted.
 *
 * Picks one target (random among the nearest) from mp_next_demotion_nodemask().
 *
 * Return: target nid on success, or NUMA_NO_NODE if no candidate is available.
 */
int mp_next_demotion_node(int nid)
{
	int target_nid;
	nodemask_t target_nodemask;

	if (mp_next_demotion_nodemask(nid, &target_nodemask))
		return NUMA_NO_NODE;
	if (nodes_empty(target_nodemask))
		return NUMA_NO_NODE;

	target_nid = node_random(&target_nodemask);

	return target_nid;
}
EXPORT_SYMBOL_GPL(mp_next_demotion_node);

/**
 * mp_next_promotion_nodemask - Promotion candidates within a package
 * @nid: Memory-only node towards which promotion seeks CPU locality.
 * @out: Output nodemask of nearest CPU targets in the same package.
 *
 * Return: 0 on success; negative errno if @nid is invalid or not initialized.
 */
int mp_next_promotion_nodemask(int nid, nodemask_t *out)
{
	return __mp_get_preferred_nodemask(nid, MP_NODES_MEM_ONLY, out);
}
EXPORT_SYMBOL_GPL(mp_next_promotion_nodemask);

/**
 * mp_next_promotion_node - Pick one promotion target
 * @nid: Memory-only node to be promoted towards CPUs.
 *
 * Picks one target (random among the nearest) from mp_next_promotion_nodemask().
 *
 * Return: target nid on success, or NUMA_NO_NODE if no candidate is available.
 */
int mp_next_promotion_node(int nid)
{
	int target_nid;
	nodemask_t target_nodemask;

	if (mp_next_promotion_nodemask(nid, &target_nodemask))
		return NUMA_NO_NODE;
	if (nodes_empty(target_nodemask))
		return NUMA_NO_NODE;

	target_nid = node_random(&target_nodemask);

	return target_nid;
}
EXPORT_SYMBOL_GPL(mp_next_promotion_node);
#endif /* CONFIG_MIGRATION */

/**
 * mp_get_package_nodes - Return all members of @nid's package
 * @nid: Any NUMA node in the package.
 * @out: Output nodemask to receive all members.
 *
 * Return: 0 on success; negative errno if @nid is invalid or not initialized.
 */
int mp_get_package_nodes(int nid, nodemask_t *out)
{
	return __mp_get_package_nodemask(nid, MP_NODES_ALL, out);
}
EXPORT_SYMBOL_GPL(mp_get_package_nodes);

/**
 * mp_get_package_cpu_nodes - Return CPU members of @nid's package
 * @nid: Any NUMA node in the package.
 * @out: Output nodemask to receive CPU members.
 *
 * Return: 0 on success; negative errno if @nid is invalid or not initialized.
 */
int mp_get_package_cpu_nodes(int nid, nodemask_t *out)
{
	return __mp_get_package_nodemask(nid, MP_NODES_CPU, out);
}
EXPORT_SYMBOL_GPL(mp_get_package_cpu_nodes);

int mp_get_package_memory_only_nodes(int nid, nodemask_t *out)
{
	return __mp_get_package_nodemask(nid, MP_NODES_MEM_ONLY, out);
}
EXPORT_SYMBOL_GPL(mp_get_package_memory_only_nodes);

/**
 * mp_get_package_memory_only_nodes - Return memory-only members of @nid's package
 * @nid: Any NUMA node in the package.
 * @out: Output nodemask to receive memory-only members.
 *
 * Return: 0 on success; negative errno if @nid is invalid or not initialized.
 */
static int __meminit mp_hotplug_callback(struct notifier_block *nb,
		unsigned long action, void *_arg)
{
	int nid;
	struct node_notify *nn = _arg;

	nid = nn->nid;
	if (nid < 0)
		return notifier_from_errno(0);

	switch (action) {
	case NODE_REMOVED_LAST_MEMORY:
		destroy_package_node(nid);
		break;

	case NODE_ADDED_FIRST_MEMORY:
		create_node_with_package(nid);
		break;

	default:
		break;
	}

	return notifier_from_errno(0);
}

static int __init memory_package_init(void)
{
	int ret = 0, nid;

	for_each_online_node(nid) {
		if (!node_state(nid, N_MEMORY))
			continue;

		/*
		 * On boot, enumerate already-present NUMA nodes and build the
		 * initial package topology. CPU nodes are the common case,
		 * but memory-only nodes are handled as well.
		 */
		ret = create_node_with_package(nid);
		if (ret) {
			pr_err("create nid(%d) failed: %d\n", nid, ret);
			goto out;
		}
	}

	hotplug_node_notifier(mp_hotplug_callback, MEMTIER_HOTPLUG_PRI);

out:
	return ret;
}
late_initcall(memory_package_init);
