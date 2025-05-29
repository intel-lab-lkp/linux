// SPDX-License-Identifier: GPL-2.0
/*
 *
 * Generic netlink for energy model.
 *
 * Copyright (c) 2025 Valve Corporation.
 * Author: Changwoo Min <changwoo@igalia.com>
 */

#define pr_fmt(fmt) "energy_model: " fmt

#include <linux/energy_model.h>
#include <net/sock.h>
#include <net/genetlink.h>
#include <uapi/linux/energy_model.h>

#include "em_netlink.h"

static const struct genl_multicast_group em_genl_mcgrps[] = {
	[EM_GENL_EVENT_GROUP]  = { .name = EM_GENL_EVENT_GROUP_NAME,  },
};

static const struct nla_policy em_genl_policy[EM_GENL_ATTR_MAX + 1] = {
};

static struct genl_family em_genl_family;


static int em_genl_cmd_doit(struct sk_buff *skb, struct genl_info *info)
{
	return -ENOTSUPP;
}

static const struct genl_small_ops em_genl_ops[] = {
	{
		.cmd = EM_GENL_CMD_PD_GET_ID,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = em_genl_cmd_doit,
	},
	{
		.cmd = EM_GENL_CMD_PD_GET_TBL,
		.validate = GENL_DONT_VALIDATE_STRICT | GENL_DONT_VALIDATE_DUMP,
		.doit = em_genl_cmd_doit,
	},
};

static struct genl_family em_genl_family __ro_after_init = {
	.hdrsize	= 0,
	.name		= EM_GENL_FAMILY_NAME,
	.version	= EM_GENL_VERSION,
	.maxattr	= EM_GENL_ATTR_MAX,
	.policy		= em_genl_policy,
	.small_ops	= em_genl_ops,
	.n_small_ops	= ARRAY_SIZE(em_genl_ops),
	.resv_start_op	= __EM_GENL_CMD_MAX,
	.mcgrps		= em_genl_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(em_genl_mcgrps),
};

int __init em_netlink_init(void)
{
	return genl_register_family(&em_genl_family);
}

void __init em_netlink_exit(void)
{
	genl_unregister_family(&em_genl_family);
}

