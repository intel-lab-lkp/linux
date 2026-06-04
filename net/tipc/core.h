/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * net/tipc/core.h: Include file for TIPC global declarations
 *
 * Copyright (c) 2005-2006, 2013-2018 Ericsson AB
 * Copyright (c) 2005-2007, 2010-2013, Wind River Systems
 * Copyright (c) 2020, Red Hat Inc
 * All rights reserved.
 */

#ifndef _TIPC_CORE_H
#define _TIPC_CORE_H

#include <linux/tipc.h>
#include <linux/tipc_config.h>
#include <linux/tipc_netlink.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/hex.h>
#include <linux/mm.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/atomic.h>
#include <linux/netdevice.h>
#include <linux/in.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/rtnetlink.h>
#include <linux/etherdevice.h>
#include <net/netns/generic.h>
#include <linux/rhashtable.h>
#include <net/genetlink.h>
#include <net/netns/hash.h>

#ifdef pr_fmt
#undef pr_fmt
#endif

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

struct tipc_node;
struct tipc_bearer;
struct tipc_bc_base;
struct tipc_link;
struct tipc_topsrv;
struct tipc_monitor;
#ifdef CONFIG_TIPC_CRYPTO
struct tipc_crypto;
#endif

#define TIPC_MOD_VER "2.0.0"

#define NODE_HTABLE_SIZE       512
#define MAX_BEARERS	         3
#define TIPC_DEF_MON_THRESHOLD  32
#define NODE_ID_LEN             16
#define NODE_ID_STR_LEN        (NODE_ID_LEN * 2 + 1)

extern unsigned int tipc_net_id __read_mostly;
extern int sysctl_tipc_rmem[3] __read_mostly;
extern int sysctl_tipc_named_timeout __read_mostly;

struct tipc_net {
	u8  node_id[NODE_ID_LEN];
	u32 node_addr;
	u32 trial_addr;
	unsigned long addr_trial_end;
	char node_id_string[NODE_ID_STR_LEN];
	int net_id;
	int random;
	bool legacy_addr_format;

	/* Node table and node list */
	spinlock_t node_list_lock;
	struct hlist_head node_htable[NODE_HTABLE_SIZE];
	struct list_head node_list;
	u32 num_nodes;
	u32 num_links;

	/* Neighbor monitoring list */
	struct tipc_monitor *monitors[MAX_BEARERS];
	int mon_threshold;

	/* Bearer list */
	struct tipc_bearer __rcu *bearer_list[MAX_BEARERS + 1];

	/* Broadcast link */
	spinlock_t bclock;
	struct tipc_bc_base *bcbase;
	struct tipc_link *bcl;

	/* Socket hash table */
	struct rhashtable sk_rht;

	/* Name table */
	spinlock_t nametbl_lock;
	struct name_table *nametbl;

	/* Topology subscription server */
	struct tipc_topsrv *topsrv;
	atomic_t subscription_count;

	/* Cluster capabilities */
	u16 capabilities;

	/* Tracing of node internal messages */
	struct packet_type loopback_pt;

#ifdef CONFIG_TIPC_CRYPTO
	/* TX crypto handler */
	struct tipc_crypto *crypto_tx;
#endif
	/* Work item for net finalize */
	struct work_struct work;
	/* The numbers of work queues in schedule */
	atomic_t wq_count;
};

static inline struct tipc_net *tipc_net(struct net *net)
{
	return net_generic(net, tipc_net_id);
}

static inline int tipc_netid(struct net *net)
{
	return tipc_net(net)->net_id;
}

static inline struct list_head *tipc_nodes(struct net *net)
{
	return &tipc_net(net)->node_list;
}

static inline struct name_table *tipc_name_table(struct net *net)
{
	return tipc_net(net)->nametbl;
}

static inline struct tipc_topsrv *tipc_topsrv(struct net *net)
{
	return tipc_net(net)->topsrv;
}

static inline unsigned int tipc_hashfn(u32 addr)
{
	return addr & (NODE_HTABLE_SIZE - 1);
}

static inline u16 mod(u16 x)
{
	return x & 0xffffu;
}

static inline int less_eq(u16 left, u16 right)
{
	return mod(right - left) < 32768u;
}

static inline int more(u16 left, u16 right)
{
	return !less_eq(left, right);
}

static inline int less(u16 left, u16 right)
{
	return less_eq(left, right) && (mod(right) != mod(left));
}

static inline int tipc_in_range(u16 val, u16 min, u16 max)
{
	return !less(val, min) && !more(val, max);
}

static inline u32 tipc_net_hash_mixes(struct net *net, int tn_rand)
{
	return net_hash_mix(&init_net) ^ net_hash_mix(net) ^ tn_rand;
}

static inline u32 hash128to32(char *bytes)
{
	__be32 *tmp = (__be32 *)bytes;
	u32 res;

	res = ntohl(tmp[0] ^ tmp[1] ^ tmp[2] ^ tmp[3]);
	if (likely(res))
		return res;
	return  ntohl(tmp[0] | tmp[1] | tmp[2] | tmp[3]);
}

#ifdef CONFIG_SYSCTL
int tipc_register_sysctl(void);
void tipc_unregister_sysctl(void);
#else
#define tipc_register_sysctl() 0
#define tipc_unregister_sysctl()
#endif
#endif
