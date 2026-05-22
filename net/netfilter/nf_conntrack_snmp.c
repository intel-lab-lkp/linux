// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *      SNMP service broadcast connection tracking helper
 *
 *      (c) 2011 Jiri Olsa <jolsa@redhat.com>
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/in.h>

#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_helper.h>
#include <net/netfilter/nf_conntrack_expect.h>
#include <linux/netfilter/nf_conntrack_snmp.h>

#define SNMP_PORT	161

MODULE_AUTHOR("Jiri Olsa <jolsa@redhat.com>");
MODULE_DESCRIPTION("SNMP service broadcast connection tracking helper");
MODULE_LICENSE("GPL");
MODULE_ALIAS_NFCT_HELPER("snmp");

static unsigned int timeout __read_mostly = 30;
module_param(timeout, uint, 0400);
MODULE_PARM_DESC(timeout, "timeout for master connection/replies in seconds");

nf_nat_snmp_hook_fn __rcu *nf_nat_snmp_hook;
EXPORT_SYMBOL_GPL(nf_nat_snmp_hook);

static int snmp_conntrack_help(struct sk_buff *skb, unsigned int protoff,
			       struct nf_conn *ct,
			       enum ip_conntrack_info ctinfo)
{
	nf_nat_snmp_hook_fn *nf_nat_snmp;

	nf_conntrack_broadcast_help(skb, ct, ctinfo, timeout);

	nf_nat_snmp = rcu_dereference(nf_nat_snmp_hook);
	if (nf_nat_snmp && ct->status & IPS_NAT_MASK)
		return nf_nat_snmp(skb, protoff, ct, ctinfo);

	return NF_ACCEPT;
}

static struct nf_conntrack_expect_policy exp_policy __ro_after_init = {
	.max_expected	= 1,
};

static struct nf_conntrack_helper helper __read_mostly = {
	.name			= "snmp",
	.nfproto		= NFPROTO_IPV4,
	.l4proto		= IPPROTO_UDP,
	.me			= THIS_MODULE,
	.help			= snmp_conntrack_help,
	.expect_policy		= &exp_policy,
};

static int __init nf_conntrack_snmp_init(void)
{
	exp_policy.timeout = timeout;
	return nf_conntrack_helper_register(&helper);
}

static void __exit nf_conntrack_snmp_fini(void)
{
	nf_conntrack_helper_unregister(&helper);
}

module_init(nf_conntrack_snmp_init);
module_exit(nf_conntrack_snmp_fini);
