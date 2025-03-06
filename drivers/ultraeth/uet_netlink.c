// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/ultraeth.yaml */
/* YNL-GEN kernel source */

#include <net/netlink.h>
#include <net/genetlink.h>

#include "uet_netlink.h"

#include <uapi/linux/ultraeth_nl.h>

/* Common nested types */
const struct nla_policy ultraeth_fep_address_nl_policy[ULTRAETH_A_FEP_ADDRESS_VERSION + 1] = {
	[ULTRAETH_A_FEP_ADDRESS_IN_ADDRESS] = NLA_POLICY_NESTED(ultraeth_fep_in_addr_nl_policy),
	[ULTRAETH_A_FEP_ADDRESS_FLAGS] = { .type = NLA_U16, },
	[ULTRAETH_A_FEP_ADDRESS_CAPS] = { .type = NLA_U16, },
	[ULTRAETH_A_FEP_ADDRESS_START_RESOURCE_INDEX] = { .type = NLA_U16, },
	[ULTRAETH_A_FEP_ADDRESS_NUM_RESOURCE_INDICES] = { .type = NLA_U16, },
	[ULTRAETH_A_FEP_ADDRESS_INITIATOR_ID] = { .type = NLA_U32, },
	[ULTRAETH_A_FEP_ADDRESS_PID_ON_FEP] = { .type = NLA_U16, },
	[ULTRAETH_A_FEP_ADDRESS_PADDING] = { .type = NLA_U16, },
	[ULTRAETH_A_FEP_ADDRESS_VERSION] = { .type = NLA_U8, },
};

const struct nla_policy ultraeth_fep_in_addr_nl_policy[ULTRAETH_A_FEP_IN_ADDR_FAMILY + 1] = {
	[ULTRAETH_A_FEP_IN_ADDR_IP] = { .type = NLA_BINARY, },
	[ULTRAETH_A_FEP_IN_ADDR_IP6] = { .type = NLA_BINARY, },
	[ULTRAETH_A_FEP_IN_ADDR_FAMILY] = { .type = NLA_U16, },
};

/* ULTRAETH_CMD_CONTEXT_NEW - do */
static const struct nla_policy ultraeth_context_new_nl_policy[ULTRAETH_A_CONTEXT_ID + 1] = {
	[ULTRAETH_A_CONTEXT_ID] = NLA_POLICY_RANGE(NLA_S32, 0, 255),
};

/* ULTRAETH_CMD_CONTEXT_DEL - do */
static const struct nla_policy ultraeth_context_del_nl_policy[ULTRAETH_A_CONTEXT_ID + 1] = {
	[ULTRAETH_A_CONTEXT_ID] = NLA_POLICY_RANGE(NLA_S32, 0, 255),
};

/* ULTRAETH_CMD_JOB_GET - dump */
static const struct nla_policy ultraeth_job_get_nl_policy[ULTRAETH_A_JOBS_CONTEXT_ID + 1] = {
	[ULTRAETH_A_JOBS_CONTEXT_ID] = { .type = NLA_S32, },
};

/* ULTRAETH_CMD_JOB_NEW - do */
static const struct nla_policy ultraeth_job_new_nl_policy[ULTRAETH_A_JOB_REQ_SERVICE_NAME + 1] = {
	[ULTRAETH_A_JOB_REQ_CONTEXT_ID] = { .type = NLA_S32, },
	[ULTRAETH_A_JOB_REQ_ID] = { .type = NLA_U32, },
	[ULTRAETH_A_JOB_REQ_ADDRESS] = NLA_POLICY_NESTED(ultraeth_fep_address_nl_policy),
	[ULTRAETH_A_JOB_REQ_SERVICE_NAME] = { .type = NLA_NUL_STRING, },
};

/* ULTRAETH_CMD_JOB_DEL - do */
static const struct nla_policy ultraeth_job_del_nl_policy[ULTRAETH_A_JOB_REQ_ID + 1] = {
	[ULTRAETH_A_JOB_REQ_CONTEXT_ID] = { .type = NLA_S32, },
	[ULTRAETH_A_JOB_REQ_ID] = { .type = NLA_U32, },
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
	{
		.cmd		= ULTRAETH_CMD_JOB_GET,
		.dumpit		= ultraeth_nl_job_get_dumpit,
		.policy		= ultraeth_job_get_nl_policy,
		.maxattr	= ULTRAETH_A_JOBS_CONTEXT_ID,
		.flags		= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= ULTRAETH_CMD_JOB_NEW,
		.doit		= ultraeth_nl_job_new_doit,
		.policy		= ultraeth_job_new_nl_policy,
		.maxattr	= ULTRAETH_A_JOB_REQ_SERVICE_NAME,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= ULTRAETH_CMD_JOB_DEL,
		.doit		= ultraeth_nl_job_del_doit,
		.policy		= ultraeth_job_del_nl_policy,
		.maxattr	= ULTRAETH_A_JOB_REQ_ID,
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
