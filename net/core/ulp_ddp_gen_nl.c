// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/ulp_ddp.yaml */
/* YNL-GEN kernel source */

#include <net/netlink.h>
#include <net/genetlink.h>

#include "ulp_ddp_gen_nl.h"

#include <uapi/linux/ulp_ddp.h>

/* ULP_DDP_CMD_CAPS_GET - do */
static const struct nla_policy ulp_ddp_caps_get_nl_policy[ULP_DDP_A_CAPS_IFINDEX + 1] = {
	[ULP_DDP_A_CAPS_IFINDEX] = { .type = NLA_U32, },
};

/* ULP_DDP_CMD_STATS_GET - do */
static const struct nla_policy ulp_ddp_stats_get_nl_policy[ULP_DDP_A_STATS_IFINDEX + 1] = {
	[ULP_DDP_A_STATS_IFINDEX] = { .type = NLA_U32, },
};

/* ULP_DDP_CMD_CAPS_SET - do */
static const struct nla_policy ulp_ddp_caps_set_nl_policy[ULP_DDP_A_CAPS_WANTED_MASK + 1] = {
	[ULP_DDP_A_CAPS_IFINDEX] = { .type = NLA_U32, },
	[ULP_DDP_A_CAPS_WANTED] = NLA_POLICY_MASK(NLA_UINT, 0x3),
	[ULP_DDP_A_CAPS_WANTED_MASK] = NLA_POLICY_MASK(NLA_UINT, 0x3),
};

/* Ops table for ulp_ddp */
static const struct genl_split_ops ulp_ddp_nl_ops[] = {
	{
		.cmd		= ULP_DDP_CMD_CAPS_GET,
		.pre_doit	= ulp_ddp_get_netdev,
		.doit		= ulp_ddp_nl_caps_get_doit,
		.post_doit	= ulp_ddp_put_netdev,
		.policy		= ulp_ddp_caps_get_nl_policy,
		.maxattr	= ULP_DDP_A_CAPS_IFINDEX,
		.flags		= GENL_CMD_CAP_DO,
	},
	{
		.cmd		= ULP_DDP_CMD_STATS_GET,
		.pre_doit	= ulp_ddp_get_netdev,
		.doit		= ulp_ddp_nl_stats_get_doit,
		.post_doit	= ulp_ddp_put_netdev,
		.policy		= ulp_ddp_stats_get_nl_policy,
		.maxattr	= ULP_DDP_A_STATS_IFINDEX,
		.flags		= GENL_CMD_CAP_DO,
	},
	{
		.cmd		= ULP_DDP_CMD_CAPS_SET,
		.pre_doit	= ulp_ddp_get_netdev,
		.doit		= ulp_ddp_nl_caps_set_doit,
		.post_doit	= ulp_ddp_put_netdev,
		.policy		= ulp_ddp_caps_set_nl_policy,
		.maxattr	= ULP_DDP_A_CAPS_WANTED_MASK,
		.flags		= GENL_CMD_CAP_DO,
	},
};

static const struct genl_multicast_group ulp_ddp_nl_mcgrps[] = {
	[ULP_DDP_NLGRP_MGMT] = { "mgmt", },
};

struct genl_family ulp_ddp_nl_family __ro_after_init = {
	.name		= ULP_DDP_FAMILY_NAME,
	.version	= ULP_DDP_FAMILY_VERSION,
	.netnsok	= true,
	.parallel_ops	= true,
	.module		= THIS_MODULE,
	.split_ops	= ulp_ddp_nl_ops,
	.n_split_ops	= ARRAY_SIZE(ulp_ddp_nl_ops),
	.mcgrps		= ulp_ddp_nl_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(ulp_ddp_nl_mcgrps),
};
