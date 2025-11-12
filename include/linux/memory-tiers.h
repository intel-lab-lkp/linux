/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MEMORY_TIERS_H
#define _LINUX_MEMORY_TIERS_H

#include <linux/types.h>
#include <linux/nodemask.h>
#include <linux/kref.h>
#include <linux/mmzone.h>
#include <linux/notifier.h>
/*
 * Each tier cover a abstrace distance chunk size of 128
 */
#define MEMTIER_CHUNK_BITS	7
#define MEMTIER_CHUNK_SIZE	(1 << MEMTIER_CHUNK_BITS)
/*
 * Smaller abstract distance values imply faster (higher) memory tiers. Offset
 * the DRAM adistance so that we can accommodate devices with a slightly lower
 * adistance value (slightly faster) than default DRAM adistance to be part of
 * the same memory tier.
 */
#define MEMTIER_ADISTANCE_DRAM	((4L * MEMTIER_CHUNK_SIZE) + (MEMTIER_CHUNK_SIZE >> 1))

struct memory_tier;
struct memory_dev_type {
	/* list of memory types that are part of same tier as this type */
	struct list_head tier_sibling;
	/* list of memory types that are managed by one driver */
	struct list_head list;
	/* abstract distance for this specific memory type */
	int adistance;
	/* Nodes of same abstract distance */
	nodemask_t nodes;
	struct kref kref;
};

struct access_coordinate;

enum {
	MT_NODE_TYPE_SYSRAM,
	MT_NODE_TYPE_SPM
};

#ifdef CONFIG_NUMA
extern bool numa_demotion_enabled;
extern struct memory_dev_type *default_dram_type;
extern nodemask_t default_dram_nodes;
extern nodemask_t mt_sysram_nodelist;
extern nodemask_t mt_spm_nodelist;
static inline nodemask_t *mt_sysram_nodemask(void)
{
	if (nodes_empty(mt_sysram_nodelist))
		return NULL;
	return &mt_sysram_nodelist;
}
static inline void mt_nodemask_sysram_mask(nodemask_t *dst, nodemask_t *mask)
{
	/* If the sysram filter isn't available, this allows all */
	if (nodes_empty(mt_sysram_nodelist)) {
		nodes_or(*dst, *mask, NODE_MASK_NONE);
		return;
	}
	nodes_and(*dst, *mask, mt_sysram_nodelist);
}
static inline bool mt_node_is_sysram(int nid)
{
	/* if sysram filter isn't setup, this allows all */
	return nodes_empty(mt_sysram_nodelist) ||
	       node_isset(nid, mt_sysram_nodelist);
}
static inline bool mt_node_allowed(int nid, gfp_t gfp_mask)
{
	if (gfp_mask & __GFP_SPM_NODE)
		return true;
	return mt_node_is_sysram(nid);
}
struct memory_dev_type *alloc_memory_type(int adistance);
void put_memory_type(struct memory_dev_type *memtype);
void init_node_memory_type(int node, struct memory_dev_type *default_type);
void clear_node_memory_type(int node, struct memory_dev_type *memtype);
int register_mt_adistance_algorithm(struct notifier_block *nb);
int unregister_mt_adistance_algorithm(struct notifier_block *nb);
int mt_calc_adistance(int node, int *adist);
int mt_set_default_dram_perf(int nid, struct access_coordinate *perf,
			     const char *source);
int mt_perf_to_adistance(struct access_coordinate *perf, int *adist);
struct memory_dev_type *mt_find_alloc_memory_type(int adist,
						  struct list_head *memory_types);
void mt_put_memory_types(struct list_head *memory_types);
#ifdef CONFIG_MIGRATION
int next_demotion_node(int node);
void node_get_allowed_targets(pg_data_t *pgdat, nodemask_t *targets);
bool node_is_toptier(int node);
#else
static inline int next_demotion_node(int node)
{
	return NUMA_NO_NODE;
}

static inline void node_get_allowed_targets(pg_data_t *pgdat, nodemask_t *targets)
{
	*targets = NODE_MASK_NONE;
}

static inline bool node_is_toptier(int node)
{
	return true;
}
#endif

int mt_set_node_type(int node, int type);

#else

#define numa_demotion_enabled	false
#define default_dram_type	NULL
#define default_dram_nodes	NODE_MASK_NONE
#define mt_sysram_nodelist	NODE_MASK_NONE
#define mt_spm_nodelist		NODE_MASK_NONE
static inline nodemask_t *mt_sysram_nodemask(void) { return NULL; }
static inline void mt_nodemask_sysram_mask(nodemask_t *dst, nodemask_t *mask) {}
static inline bool mt_node_is_sysram(int nid) { return true; }
static inline bool mt_node_allowed(int nid, gfp_t gfp_mask) { return true; }
/*
 * CONFIG_NUMA implementation returns non NULL error.
 */
static inline struct memory_dev_type *alloc_memory_type(int adistance)
{
	return NULL;
}

static inline void put_memory_type(struct memory_dev_type *memtype)
{

}

static inline void init_node_memory_type(int node, struct memory_dev_type *default_type)
{

}

static inline void clear_node_memory_type(int node, struct memory_dev_type *memtype)
{

}

static inline int next_demotion_node(int node)
{
	return NUMA_NO_NODE;
}

static inline void node_get_allowed_targets(pg_data_t *pgdat, nodemask_t *targets)
{
	*targets = NODE_MASK_NONE;
}

static inline bool node_is_toptier(int node)
{
	return true;
}

static inline int register_mt_adistance_algorithm(struct notifier_block *nb)
{
	return 0;
}

static inline int unregister_mt_adistance_algorithm(struct notifier_block *nb)
{
	return 0;
}

static inline int mt_calc_adistance(int node, int *adist)
{
	return NOTIFY_DONE;
}

static inline int mt_set_default_dram_perf(int nid, struct access_coordinate *perf,
					   const char *source)
{
	return -EIO;
}

static inline int mt_perf_to_adistance(struct access_coordinate *perf, int *adist)
{
	return -EIO;
}

static inline struct memory_dev_type *mt_find_alloc_memory_type(int adist,
								struct list_head *memory_types)
{
	return NULL;
}

static inline void mt_put_memory_types(struct list_head *memory_types)
{
}

int mt_set_node_type(int node, int type)
{
	return 0;
}
#endif	/* CONFIG_NUMA */
#endif  /* _LINUX_MEMORY_TIERS_H */
