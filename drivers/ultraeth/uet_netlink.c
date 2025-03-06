// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/ultraeth.yaml */
/* YNL-GEN kernel source */

#include <net/netlink.h>
#include <net/genetlink.h>

#include "uet_netlink.h"

#include <uapi/linux/ultraeth_nl.h>

/* ULTRAETH_CMD_CONTEXT_NEW - do */
static const struct nla_policy ultraeth_context_new_nl_policy[ULTRAETH_A_CONTEXT_ID + 1] = {
	[ULTRAETH_A_CONTEXT_ID] = NLA_POLICY_RANGE(NLA_S32, 0, 255),
};

/* ULTRAETH_CMD_CONTEXT_DEL - do */
static const struct nla_policy ultraeth_context_del_nl_policy[ULTRAETH_A_CONTEXT_ID + 1] = {
	[ULTRAETH_A_CONTEXT_ID] = NLA_POLICY_RANGE(NLA_S32, 0, 255),
};

/* Ops table for ultraeth */
static const struct genl_split_ops ultraeth_nl_ops[] = {
	{
		.cmd	= ULTRAETH_CMD_CONTEXT_GET,
		.dumpit	= ultraeth_nl_context_get_dumpit,
		.flags	= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= ULTRAETH_CMD_CONTEXT_NEW,
		.doit		= ultraeth_nl_context_new_doit,
		.policy		= ultraeth_context_new_nl_policy,
		.maxattr	= ULTRAETH_A_CONTEXT_ID,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= ULTRAETH_CMD_CONTEXT_DEL,
		.doit		= ultraeth_nl_context_del_doit,
		.policy		= ultraeth_context_del_nl_policy,
		.maxattr	= ULTRAETH_A_CONTEXT_ID,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
};

struct genl_family ultraeth_nl_family __ro_after_init = {
	.name		= ULTRAETH_FAMILY_NAME,
	.version	= ULTRAETH_FAMILY_VERSION,
	.netnsok	= true,
	.parallel_ops	= true,
	.module		= THIS_MODULE,
	.split_ops	= ultraeth_nl_ops,
	.n_split_ops	= ARRAY_SIZE(ultraeth_nl_ops),
};
