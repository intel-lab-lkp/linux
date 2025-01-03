// SPDX-License-Identifier: GPL-2.0-only
/* IP tables module for matching and setting the value of the IPv4/IPv6 DSCP field
 *
 * (C) 2002 by Harald Welte <laforge@netfilter.org>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <net/dsfield.h>

#include <linux/netfilter/x_tables.h>
#include <linux/netfilter/xt_dscp.h>

#define XT_DSCP_ECN_MASK	3u

MODULE_AUTHOR("Harald Welte <laforge@netfilter.org>");
MODULE_DESCRIPTION("Xtables: DSCP/TOS field match and modification target");
MODULE_LICENSE("GPL");
MODULE_ALIAS("ipt_dscp");
MODULE_ALIAS("ip6t_dscp");
MODULE_ALIAS("ipt_tos");
MODULE_ALIAS("ip6t_tos");
MODULE_ALIAS("ipt_DSCP");
MODULE_ALIAS("ip6t_DSCP");
MODULE_ALIAS("ipt_TOS");
MODULE_ALIAS("ip6t_TOS");
MODULE_ALIAS("xt_DSCP");

static bool
dscp_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_dscp_info *info = par->matchinfo;
	u_int8_t dscp = ipv4_get_dsfield(ip_hdr(skb)) >> XT_DSCP_SHIFT;

	return (dscp == info->dscp) ^ !!info->invert;
}

static bool
dscp_mt6(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_dscp_info *info = par->matchinfo;
	u_int8_t dscp = ipv6_get_dsfield(ipv6_hdr(skb)) >> XT_DSCP_SHIFT;

	return (dscp == info->dscp) ^ !!info->invert;
}

static int dscp_mt_check(const struct xt_mtchk_param *par)
{
	const struct xt_dscp_info *info = par->matchinfo;

	if (info->dscp > XT_DSCP_MAX)
		return -EDOM;

	return 0;
}

static bool tos_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_tos_match_info *info = par->matchinfo;

	if (xt_family(par) == NFPROTO_IPV4)
		return ((ip_hdr(skb)->tos & info->tos_mask) ==
		       info->tos_value) ^ !!info->invert;
	else
		return ((ipv6_get_dsfield(ipv6_hdr(skb)) & info->tos_mask) ==
		       info->tos_value) ^ !!info->invert;
}

static struct xt_match dscp_mt_reg[] __read_mostly = {
	{
		.name		= "dscp",
		.family		= NFPROTO_IPV4,
		.checkentry	= dscp_mt_check,
		.match		= dscp_mt,
		.matchsize	= sizeof(struct xt_dscp_info),
		.me		= THIS_MODULE,
	},
	{
		.name		= "dscp",
		.family		= NFPROTO_IPV6,
		.checkentry	= dscp_mt_check,
		.match		= dscp_mt6,
		.matchsize	= sizeof(struct xt_dscp_info),
		.me		= THIS_MODULE,
	},
	{
		.name		= "tos",
		.revision	= 1,
		.family		= NFPROTO_IPV4,
		.match		= tos_mt,
		.matchsize	= sizeof(struct xt_tos_match_info),
		.me		= THIS_MODULE,
	},
	{
		.name		= "tos",
		.revision	= 1,
		.family		= NFPROTO_IPV6,
		.match		= tos_mt,
		.matchsize	= sizeof(struct xt_tos_match_info),
		.me		= THIS_MODULE,
	},
};

static unsigned int
dscp_tg(struct sk_buff *skb, const struct xt_action_param *par)
{
	const struct xt_DSCP_info *dinfo = par->targinfo;
	u8 dscp = ipv4_get_dsfield(ip_hdr(skb)) >> XT_DSCP_SHIFT;

	if (dscp != dinfo->dscp) {
		if (skb_ensure_writable(skb, sizeof(struct iphdr)))
			return NF_DROP;

		ipv4_change_dsfield(ip_hdr(skb), XT_DSCP_ECN_MASK,
				    dinfo->dscp << XT_DSCP_SHIFT);
	}
	return XT_CONTINUE;
}

static unsigned int
dscp_tg6(struct sk_buff *skb, const struct xt_action_param *par)
{
	const struct xt_DSCP_info *dinfo = par->targinfo;
	u8 dscp = ipv6_get_dsfield(ipv6_hdr(skb)) >> XT_DSCP_SHIFT;

	if (dscp != dinfo->dscp) {
		if (skb_ensure_writable(skb, sizeof(struct ipv6hdr)))
			return NF_DROP;

		ipv6_change_dsfield(ipv6_hdr(skb), XT_DSCP_ECN_MASK,
				    dinfo->dscp << XT_DSCP_SHIFT);
	}
	return XT_CONTINUE;
}

static int dscp_tg_check(const struct xt_tgchk_param *par)
{
	return xt_register_matches(dscp_mt_reg, ARRAY_SIZE(dscp_mt_reg));
	const struct xt_DSCP_info *info = par->targinfo;

	if (info->dscp > XT_DSCP_MAX)
		return -EDOM;
	return 0;
}

static unsigned int
tos_tg(struct sk_buff *skb, const struct xt_action_param *par)
{
	const struct xt_tos_target_info *info = par->targinfo;
	struct iphdr *iph = ip_hdr(skb);
	u8 orig, nv;

	orig = ipv4_get_dsfield(iph);
	nv   = (orig & ~info->tos_mask) ^ info->tos_value;

	if (orig != nv) {
		if (skb_ensure_writable(skb, sizeof(struct iphdr)))
			return NF_DROP;
		iph = ip_hdr(skb);
		ipv4_change_dsfield(iph, 0, nv);
	}

	return XT_CONTINUE;
}

static unsigned int
tos_tg6(struct sk_buff *skb, const struct xt_action_param *par)
{
	const struct xt_tos_target_info *info = par->targinfo;
	struct ipv6hdr *iph = ipv6_hdr(skb);
	u8 orig, nv;

	orig = ipv6_get_dsfield(iph);
	nv   = (orig & ~info->tos_mask) ^ info->tos_value;

	if (orig != nv) {
		if (skb_ensure_writable(skb, sizeof(struct iphdr)))
			return NF_DROP;
		iph = ipv6_hdr(skb);
		ipv6_change_dsfield(iph, 0, nv);
	}

	return XT_CONTINUE;
}

static struct xt_target dscp_tg_reg[] __read_mostly = {
	{
		.name		= "DSCP",
		.family		= NFPROTO_IPV4,
		.checkentry	= dscp_tg_check,
		.target		= dscp_tg,
		.targetsize	= sizeof(struct xt_DSCP_info),
		.table		= "mangle",
		.me		= THIS_MODULE,
	},
	{
		.name		= "DSCP",
		.family		= NFPROTO_IPV6,
		.checkentry	= dscp_tg_check,
		.target		= dscp_tg6,
		.targetsize	= sizeof(struct xt_DSCP_info),
		.table		= "mangle",
		.me		= THIS_MODULE,
	},
	{
		.name		= "TOS",
		.revision	= 1,
		.family		= NFPROTO_IPV4,
		.table		= "mangle",
		.target		= tos_tg,
		.targetsize	= sizeof(struct xt_tos_target_info),
		.me		= THIS_MODULE,
	},
	{
		.name		= "TOS",
		.revision	= 1,
		.family		= NFPROTO_IPV6,
		.table		= "mangle",
		.target		= tos_tg6,
		.targetsize	= sizeof(struct xt_tos_target_info),
		.me		= THIS_MODULE,
	},
};

static int __init dscp_init(void)
{
	int ret;

	ret = xt_register_targets(dscp_tg_reg, ARRAY_SIZE(dscp_tg_reg));
	if (ret < 0)
		return ret;
	ret = xt_register_matches(dscp_mt_reg, ARRAY_SIZE(dscp_mt_reg));
	if (ret < 0) {
		xt_unregister_targets(dscp_tg_reg, ARRAY_SIZE(dscp_tg_reg));
		return ret;
	}
	return 0;
}

static void __exit dscp_exit(void)
{
	xt_unregister_matches(dscp_mt_reg, ARRAY_SIZE(dscp_mt_reg));
	xt_unregister_targets(dscp_tg_reg, ARRAY_SIZE(dscp_tg_reg));
}

module_init(dscp_init);
module_exit(dscp_exit);
