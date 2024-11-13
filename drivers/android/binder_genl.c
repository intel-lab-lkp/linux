// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/binder_genl.yaml */
/* YNL-GEN kernel source */

#include <net/netlink.h>
#include <net/genetlink.h>

#include "binder_genl.h"

#include <uapi/linux/android/binder_genl.h>

/* BINDER_GENL_CMD_SET - do */
static const struct nla_policy binder_genl_set_nl_policy[BINDER_GENL_A_CMD_FLAGS + 1] = {
	[BINDER_GENL_A_CMD_CONTEXT] = { .type = NLA_NUL_STRING, },
	[BINDER_GENL_A_CMD_PID] = { .type = NLA_U32, },
	[BINDER_GENL_A_CMD_FLAGS] = NLA_POLICY_MASK(NLA_U32, 0xf),
};

/* Ops table for binder_genl */
static const struct genl_split_ops binder_genl_nl_ops[] = {
	{
		.cmd		= BINDER_GENL_CMD_SET,
		.doit		= binder_genl_nl_set_doit,
		.policy		= binder_genl_set_nl_policy,
		.maxattr	= BINDER_GENL_A_CMD_FLAGS,
		.flags		= GENL_CMD_CAP_DO,
	},
};

struct genl_family binder_genl_nl_family __ro_after_init = {
	.name		= BINDER_GENL_FAMILY_NAME,
	.version	= BINDER_GENL_FAMILY_VERSION,
	.netnsok	= true,
	.parallel_ops	= true,
	.module		= THIS_MODULE,
	.split_ops	= binder_genl_nl_ops,
	.n_split_ops	= ARRAY_SIZE(binder_genl_nl_ops),
};
