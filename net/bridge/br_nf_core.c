// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *	Handle firewalling core
 *	Linux ethernet bridge
 *
 *	Authors:
 *	Lennert Buytenhek		<buytenh@gnu.org>
 *	Bart De Schuymer		<bdschuym@pandora.be>
 *
 *	Lennert dedicates this file to Kerstin Wurdinger.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/in_route.h>
#include <linux/inetdevice.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <net/route.h>

#include "br_private.h"
#ifdef CONFIG_SYSCTL
#include <linux/sysctl.h>
#endif

static void fake_update_pmtu(struct dst_entry *dst, struct sock *sk,
			     struct sk_buff *skb, u32 mtu,
			     bool confirm_neigh)
{
}

static void fake_redirect(struct dst_entry *dst, struct sock *sk,
			  struct sk_buff *skb)
{
}

static u32 *fake_cow_metrics(struct dst_entry *dst, unsigned long old)
{
	return NULL;
}

static struct neighbour *fake_neigh_lookup(const struct dst_entry *dst,
					   struct sk_buff *skb,
					   const void *daddr)
{
	return NULL;
}

static unsigned int fake_mtu(const struct dst_entry *dst)
{
	return dst->dev->mtu;
}

struct br_fake_rtable {
	struct rtable	rt;
	u32		metrics[RTAX_MAX];
};

static struct dst_ops fake_dst_ops = {
	.family		= AF_INET,
	.update_pmtu	= fake_update_pmtu,
	.redirect	= fake_redirect,
	.cow_metrics	= fake_cow_metrics,
	.neigh_lookup	= fake_neigh_lookup,
	.mtu		= fake_mtu,
};

/*
 * Initialize bogus route table used to keep netfilter happy.
 * Currently, we fill in the PMTU entry because netfilter
 * refragmentation needs it, and the rt_flags entry because
 * ipt_REJECT needs it.  Future netfilter modules might
 * require us to fill additional fields.
 */
int br_netfilter_rtable_init(struct net_bridge *br)
{
	struct br_fake_rtable *fake_rt;
	struct rtable *rt;

	fake_rt = kmem_cache_zalloc(fake_dst_ops.kmem_cachep, GFP_KERNEL);
	if (!fake_rt)
		return -ENOMEM;

	rt = &fake_rt->rt;
	dst_init(&rt->dst, &fake_dst_ops, br->dev, DST_OBSOLETE_NONE,
		 DST_NOXFRM | DST_FAKE_RTABLE);
	dst_init_metrics(&rt->dst, fake_rt->metrics, false);
	dst_metric_set(&rt->dst, RTAX_MTU, br->dev->mtu);
	rcu_assign_pointer(br->fake_rtable, rt);

	return 0;
}

void br_netfilter_rtable_fini(struct net_bridge *br)
{
	struct rtable *rt;

	rt = rcu_replace_pointer(br->fake_rtable, NULL, lockdep_rtnl_is_held());
	if (rt)
		dst_release(&rt->dst);
}

int __init br_nf_core_init(void)
{
	int err;

	fake_dst_ops.kmem_cachep =
		KMEM_CACHE(br_fake_rtable, SLAB_HWCACHE_ALIGN | SLAB_PANIC);
	err = dst_entries_init(&fake_dst_ops);
	if (err)
		fake_dst_ops.kmem_cachep = NULL;

	return err;
}

void br_nf_core_fini(void)
{
	dst_entries_destroy(&fake_dst_ops);
	kmem_cache_destroy(fake_dst_ops.kmem_cachep);
	fake_dst_ops.kmem_cachep = NULL;
}
