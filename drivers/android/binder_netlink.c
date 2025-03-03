// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/binder.yaml */
/* YNL-GEN kernel source */

#include <net/netlink.h>
#include <net/genetlink.h>

#include "binder_netlink.h"

#include <uapi/linux/android/binder_netlink.h>
#include <binder_internal.h>

/* BINDER_CMD_REPORT_SETUP - do */
static const struct nla_policy binder_report_setup_nl_policy[BINDER_A_CMD_FLAGS + 1] = {
	[BINDER_A_CMD_CONTEXT] = { .type = NLA_NUL_STRING, },
	[BINDER_A_CMD_PID] = { .type = NLA_U32, },
	[BINDER_A_CMD_FLAGS] = NLA_POLICY_MASK(NLA_U32, 0xf),
};

/* Ops table for binder */
static const struct genl_split_ops binder_nl_ops[] = {
	{
		.cmd		= BINDER_CMD_REPORT_SETUP,
		.doit		= binder_nl_report_setup_doit,
		.policy		= binder_report_setup_nl_policy,
		.maxattr	= BINDER_A_CMD_FLAGS,
		.flags		= GENL_CMD_CAP_DO,
	},
};

static const struct genl_multicast_group binder_nl_mcgrps[] = {
	[BINDER_NLGRP_REPORT] = { "report", },
};

struct genl_family binder_nl_family __ro_after_init = {
	.name		= BINDER_FAMILY_NAME,
	.version	= BINDER_FAMILY_VERSION,
	.netnsok	= true,
	.parallel_ops	= true,
	.module		= THIS_MODULE,
	.split_ops	= binder_nl_ops,
	.n_split_ops	= ARRAY_SIZE(binder_nl_ops),
	.mcgrps		= binder_nl_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(binder_nl_mcgrps),
};
