/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2007-2013 Nicira, Inc.
 */

#ifndef FLOW_TABLE_H
#define FLOW_TABLE_H 1

#include <linux/kernel.h>
#include <linux/netlink.h>
#include <linux/openvswitch.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/rcupdate.h>
#include <linux/if_ether.h>
#include <linux/in6.h>
#include <linux/jiffies.h>
#include <linux/time.h>

#include <net/inet_ecn.h>
#include <net/ip_tunnels.h>

#include "flow.h"

struct mask_cache_entry {
	u32 skb_hash;
	u32 mask_index;
};

struct mask_cache {
	struct rcu_head rcu;
	u32 cache_size;  /* Must be ^2 value. */
	struct mask_cache_entry __percpu *mask_cache;
};

struct mask_count {
	int index;
	u64 counter;
};

struct mask_array_stats {
	struct u64_stats_sync syncp;
	u64 usage_cntrs[];
};

struct mask_array {
	struct rcu_head rcu;
	int count, max;
	struct mask_array_stats __percpu *masks_usage_stats;
	u64 *masks_usage_zero_cntr;
	struct sw_flow_mask __rcu *masks[] __counted_by(max);
};

struct table_instance {
	struct hlist_head *buckets;
	unsigned int n_buckets;
	struct rcu_head rcu;
	int node_ver;
	u32 hash_seed;
};

/* Locking:
 *
 * flow_table is _not_ protected by ovs_lock (see comment above ovs_mutex
 * in datapath.c).
 *
 * All writes to flow_table are protected by the embedded "lock".
 * In order to ensure datapath destruction does not trigger the destruction
 * of the flow_table, "refcnt" is used. Therefore, writers must:
 * 1 - Enter rcu read-protected section
 * 2 - Increase "table->refcnt"
 * 3 - Leave rcu read-protected section (to avoid using mutexes inside rcu)
 * 4 - Lock "table->lock"
 * 5 - Perform modifications
 * 6 - Release "table->lock"
 * 7 - Decrease "table->refcnt"
 *
 * Reads are protected by RCU.
 *
 * Note with this schema, it's possible that a flow operation is performed on a
 * flow_table that is about to be freed.
 */
struct flow_table {
	/* Locks flow table writes. */
	struct mutex lock;
	refcount_t refcnt;
	struct rcu_head rcu;
	struct table_instance __rcu *ti;
	struct table_instance __rcu *ufid_ti;
	struct mask_cache __rcu *mask_cache;
	struct mask_array __rcu *mask_array;
	unsigned long last_rehash;
	unsigned int count;
	unsigned int ufid_count;
};

extern struct kmem_cache *flow_stats_cache;

#ifdef CONFIG_LOCKDEP
int lockdep_ovs_tbl_is_held(const struct flow_table *table);
#else
static inline int lockdep_ovs_tbl_is_held(const struct flow_table *table
					  __always_unused)
{
	return 1;
}
#endif

#define ASSERT_OVS_TBL(tbl)   WARN_ON(!lockdep_ovs_tbl_is_held(tbl))

/* Lock-protected update-allowed dereferences.*/
#define ovs_tbl_dereference(p, tbl)	\
	rcu_dereference_protected(p, lockdep_ovs_tbl_is_held(tbl))

/* Read dereferences can be protected by either RCU, table lock. */
#define rcu_dereference_ovs_tbl(p, tbl) \
	rcu_dereference_check(p, lockdep_ovs_tbl_is_held(tbl))

int ovs_flow_init(void);
void ovs_flow_exit(void);

struct sw_flow *ovs_flow_alloc(void);
void ovs_flow_free(struct sw_flow *, bool deferred);

struct flow_table *ovs_flow_tbl_alloc(void);
void ovs_flow_tbl_put(struct flow_table *table);
static inline bool ovs_flow_tbl_get(struct flow_table *table)
{
	return refcount_inc_not_zero(&table->refcnt);
}
int ovs_flow_tbl_count(const struct flow_table *table);
int ovs_flow_tbl_flush(struct flow_table *flow_table);

int ovs_flow_tbl_insert(struct flow_table *table, struct sw_flow *flow,
			const struct sw_flow_mask *mask);
void ovs_flow_tbl_remove(struct flow_table *table, struct sw_flow *flow);
int  ovs_flow_tbl_num_masks(const struct flow_table *table);
u32  ovs_flow_tbl_masks_cache_size(const struct flow_table *table);
int  ovs_flow_tbl_masks_cache_resize(struct flow_table *table, u32 size);
struct sw_flow *ovs_flow_tbl_dump_next(struct table_instance *table,
				       u32 *bucket, u32 *idx);
struct sw_flow *ovs_flow_tbl_lookup_stats(struct flow_table *,
					  const struct sw_flow_key *,
					  u32 skb_hash,
					  u32 *n_mask_hit,
					  u32 *n_cache_hit);
struct sw_flow *ovs_flow_tbl_lookup(struct flow_table *,
				    const struct sw_flow_key *);
struct sw_flow *ovs_flow_tbl_lookup_exact(struct flow_table *tbl,
					  const struct sw_flow_match *match);
struct sw_flow *ovs_flow_tbl_lookup_ufid(struct flow_table *,
					 const struct sw_flow_id *);

bool ovs_flow_cmp(const struct sw_flow *, const struct sw_flow_match *);

void ovs_flow_mask_key(struct sw_flow_key *dst, const struct sw_flow_key *src,
		       bool full, const struct sw_flow_mask *mask);

void ovs_flow_masks_rebalance(struct flow_table *table);

#endif /* flow_table.h */
