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
#include <linux/rcupdate.h>
#include <net/route.h>

#include "br_private.h"
#ifdef CONFIG_SYSCTL
#include <linux/sysctl.h>
#endif

/*
 * Initialize bogus route table used to keep netfilter happy.
 * Currently, we fill in the PMTU entry because netfilter
 * refragmentation needs it, and the rt_flags entry because
 * ipt_REJECT needs it.  Future netfilter modules might
 * require us to fill additional fields.
 */
int br_netfilter_rtable_init(struct net_bridge *br)
{
	struct rtable *rt;

	rt = rt_dst_alloc(br->dev, 0, RTN_UNSPEC, true);
	if (!rt)
		return -ENOMEM;

	rt->dst.flags |= DST_FAKE_RTABLE;
	rcu_assign_pointer(br->fake_rtable, rt);

	return 0;
}

void br_netfilter_rtable_fini(struct net_bridge *br)
{
	struct rtable *rt;

	rt = rcu_replace_pointer(br->fake_rtable, NULL, lockdep_rtnl_is_held());
	if (!rt)
		return;

	dst_dev_put(&rt->dst);
	dst_release(&rt->dst);
}

int __init br_nf_core_init(void)
{
	return 0;
}

void br_nf_core_fini(void)
{
}
