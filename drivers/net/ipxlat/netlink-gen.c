// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/ipxlat.yaml */
/* YNL-GEN kernel source */
/* To regenerate run: tools/net/ynl/ynl-regen.sh */

#include <net/netlink.h>
#include <net/genetlink.h>

#include "netlink-gen.h"

#include <uapi/linux/ipxlat.h>

/* Common nested types */
const struct nla_policy ipxlat_cfg_nl_policy[IPXLAT_A_CFG_LOWEST_IPV6_MTU + 1] = {
	[IPXLAT_A_CFG_XLAT_PREFIX6] = NLA_POLICY_NESTED(ipxlat_pool_nl_policy),
	[IPXLAT_A_CFG_LOWEST_IPV6_MTU] = NLA_POLICY_MIN(NLA_U32, 1280),
};

const struct nla_policy ipxlat_pool_nl_policy[IPXLAT_A_POOL_PREFIX_LEN + 1] = {
	[IPXLAT_A_POOL_PREFIX] = NLA_POLICY_EXACT_LEN(16),
	[IPXLAT_A_POOL_PREFIX_LEN] = NLA_POLICY_MAX(NLA_U8, IPXLAT_XLAT_PREFIX6_MAX_PREFIX_LEN),
};

/* IPXLAT_CMD_DEV_GET - do */
static const struct nla_policy ipxlat_dev_get_nl_policy[IPXLAT_A_DEV_IFINDEX + 1] = {
	[IPXLAT_A_DEV_IFINDEX] = { .type = NLA_U32, },
};

/* IPXLAT_CMD_DEV_SET - do */
static const struct nla_policy ipxlat_dev_set_nl_policy[IPXLAT_A_DEV_CONFIG + 1] = {
	[IPXLAT_A_DEV_IFINDEX] = { .type = NLA_U32, },
	[IPXLAT_A_DEV_CONFIG] = NLA_POLICY_NESTED(ipxlat_cfg_nl_policy),
};

/* Ops table for ipxlat */
static const struct genl_split_ops ipxlat_nl_ops[] = {
	{
		.cmd		= IPXLAT_CMD_DEV_GET,
		.pre_doit	= ipxlat_nl_pre_doit,
		.doit		= ipxlat_nl_dev_get_doit,
		.post_doit	= ipxlat_nl_post_doit,
		.policy		= ipxlat_dev_get_nl_policy,
		.maxattr	= IPXLAT_A_DEV_IFINDEX,
		.flags		= GENL_CMD_CAP_DO,
	},
	{
		.cmd	= IPXLAT_CMD_DEV_GET,
		.dumpit	= ipxlat_nl_dev_get_dumpit,
		.flags	= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= IPXLAT_CMD_DEV_SET,
		.pre_doit	= ipxlat_nl_pre_doit,
		.doit		= ipxlat_nl_dev_set_doit,
		.post_doit	= ipxlat_nl_post_doit,
		.policy		= ipxlat_dev_set_nl_policy,
		.maxattr	= IPXLAT_A_DEV_CONFIG,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
};

struct genl_family ipxlat_nl_family __ro_after_init = {
	.name		= IPXLAT_FAMILY_NAME,
	.version	= IPXLAT_FAMILY_VERSION,
	.netnsok	= true,
	.parallel_ops	= true,
	.module		= THIS_MODULE,
	.split_ops	= ipxlat_nl_ops,
	.n_split_ops	= ARRAY_SIZE(ipxlat_nl_ops),
};
