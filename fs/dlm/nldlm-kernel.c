// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/nldlm.yaml */
/* YNL-GEN kernel source */

#include <net/netlink.h>
#include <net/genetlink.h>

#include "nldlm-kernel.h"

#include <uapi/linux/nldlm.h>

/* Common nested types */
const struct nla_policy nldlm_addr_nl_policy[NLDLM_A_ADDR_ADDR6 + 1] = {
	[NLDLM_A_ADDR_FAMILY] = { .type = NLA_U16, },
	[NLDLM_A_ADDR_ADDR4] = { .type = NLA_U32, },
	[NLDLM_A_ADDR_ADDR6] = NLA_POLICY_EXACT_LEN(16),
};

/* NLDLM_CMD_GET_NODE - do */
static const struct nla_policy nldlm_get_node_nl_policy[NLDLM_A_NODE_ID + 1] = {
	[NLDLM_A_NODE_ID] = { .type = NLA_U32, },
};

/* NLDLM_CMD_ADD_NODE - do */
static const struct nla_policy nldlm_add_node_nl_policy[NLDLM_A_NODE_ADDRS + 1] = {
	[NLDLM_A_NODE_ID] = { .type = NLA_U32, },
	[NLDLM_A_NODE_MARK] = { .type = NLA_U32, },
	[NLDLM_A_NODE_ADDRS] = NLA_POLICY_NESTED(nldlm_addr_nl_policy),
};

/* NLDLM_CMD_DEL_NODE - do */
static const struct nla_policy nldlm_del_node_nl_policy[NLDLM_A_NODE_ID + 1] = {
	[NLDLM_A_NODE_ID] = { .type = NLA_U32, },
};

/* NLDLM_CMD_GET_LS - do */
static const struct nla_policy nldlm_get_ls_nl_policy[NLDLM_A_LS_NAME + 1] = {
	[NLDLM_A_LS_NAME] = { .type = NLA_NUL_STRING, },
};

/* NLDLM_CMD_GET_LS_MEMBER - do */
static const struct nla_policy nldlm_get_ls_member_do_nl_policy[NLDLM_A_LS_MEMBER_NODEID + 1] = {
	[NLDLM_A_LS_MEMBER_LS_NAME] = { .type = NLA_NUL_STRING, },
	[NLDLM_A_LS_MEMBER_NODEID] = { .type = NLA_U32, },
};

/* NLDLM_CMD_GET_LS_MEMBER - dump */
static const struct nla_policy nldlm_get_ls_member_dump_nl_policy[NLDLM_A_LS_MEMBER_LS_NAME + 1] = {
	[NLDLM_A_LS_MEMBER_LS_NAME] = { .type = NLA_NUL_STRING, },
};

/* NLDLM_CMD_LS_ADD_MEMBER - do */
static const struct nla_policy nldlm_ls_add_member_nl_policy[NLDLM_A_LS_MEMBER_WEIGHT + 1] = {
	[NLDLM_A_LS_MEMBER_LS_NAME] = { .type = NLA_NUL_STRING, },
	[NLDLM_A_LS_MEMBER_NODEID] = { .type = NLA_U32, },
	[NLDLM_A_LS_MEMBER_WEIGHT] = { .type = NLA_U32, },
};

/* NLDLM_CMD_LS_DEL_MEMBER - do */
static const struct nla_policy nldlm_ls_del_member_nl_policy[NLDLM_A_LS_MEMBER_NODEID + 1] = {
	[NLDLM_A_LS_MEMBER_LS_NAME] = { .type = NLA_NUL_STRING, },
	[NLDLM_A_LS_MEMBER_NODEID] = { .type = NLA_U32, },
};

/* NLDLM_CMD_LS_CTRL - do */
static const struct nla_policy nldlm_ls_ctrl_nl_policy[NLDLM_A_LS_CTRL_ACTION + 1] = {
	[NLDLM_A_LS_CTRL_LS_NAME] = { .type = NLA_NUL_STRING, },
	[NLDLM_A_LS_CTRL_ACTION] = { .type = NLA_U32, },
};

/* NLDLM_CMD_LS_EVENT_DONE - do */
static const struct nla_policy nldlm_ls_event_done_nl_policy[NLDLM_A_LS_EVENT_RESULT_RESULT + 1] = {
	[NLDLM_A_LS_EVENT_RESULT_LS_NAME] = { .type = NLA_NUL_STRING, },
	[NLDLM_A_LS_EVENT_RESULT_LS_GLOBAL_ID] = { .type = NLA_U32, },
	[NLDLM_A_LS_EVENT_RESULT_RESULT] = { .type = NLA_U32, },
};

/* NLDLM_CMD_SET_OUR_NODEID - do */
static const struct nla_policy nldlm_set_our_nodeid_nl_policy[NLDLM_A_CFG_OUR_NODEID + 1] = {
	[NLDLM_A_CFG_OUR_NODEID] = { .type = NLA_U32, },
};

/* NLDLM_CMD_SET_CLUSTER_NAME - do */
static const struct nla_policy nldlm_set_cluster_name_nl_policy[NLDLM_A_CFG_CLUSTER_NAME + 1] = {
	[NLDLM_A_CFG_CLUSTER_NAME] = { .type = NLA_NUL_STRING, },
};

/* NLDLM_CMD_SET_PROTOCOL - do */
static const struct nla_policy nldlm_set_protocol_nl_policy[NLDLM_A_CFG_PROTOCOL + 1] = {
	[NLDLM_A_CFG_PROTOCOL] = { .type = NLA_U32, },
};

/* NLDLM_CMD_SET_PORT - do */
static const struct nla_policy nldlm_set_port_nl_policy[NLDLM_A_CFG_PORT + 1] = {
	[NLDLM_A_CFG_PORT] = { .type = NLA_U16, },
};

/* NLDLM_CMD_SET_RECOVER_TIMEOUT - do */
static const struct nla_policy nldlm_set_recover_timeout_nl_policy[NLDLM_A_CFG_RECOVER_TIMEOUT + 1] = {
	[NLDLM_A_CFG_RECOVER_TIMEOUT] = { .type = NLA_U32, },
};

/* NLDLM_CMD_SET_INACTIVE_TIMEOUT - do */
static const struct nla_policy nldlm_set_inactive_timeout_nl_policy[NLDLM_A_CFG_INACTIVE_TIMEOUT + 1] = {
	[NLDLM_A_CFG_INACTIVE_TIMEOUT] = { .type = NLA_U32, },
};

/* NLDLM_CMD_SET_LOG_LEVEL - do */
static const struct nla_policy nldlm_set_log_level_nl_policy[NLDLM_A_CFG_LOG_LEVEL + 1] = {
	[NLDLM_A_CFG_LOG_LEVEL] = { .type = NLA_U32, },
};

/* NLDLM_CMD_SET_DEFAULT_MARK - do */
static const struct nla_policy nldlm_set_default_mark_nl_policy[NLDLM_A_CFG_DEFAULT_MARK + 1] = {
	[NLDLM_A_CFG_DEFAULT_MARK] = { .type = NLA_U32, },
};

/* NLDLM_CMD_SET_RECOVER_CALLBACKS - do */
static const struct nla_policy nldlm_set_recover_callbacks_nl_policy[NLDLM_A_CFG_RECOVER_CALLBACKS + 1] = {
	[NLDLM_A_CFG_RECOVER_CALLBACKS] = { .type = NLA_FLAG, },
};

/* Ops table for nldlm */
static const struct genl_split_ops nldlm_nl_ops[] = {
	{
		.cmd		= NLDLM_CMD_GET_NODE,
		.doit		= nldlm_nl_get_node_doit,
		.policy		= nldlm_get_node_nl_policy,
		.maxattr	= NLDLM_A_NODE_ID,
		.flags		= GENL_CMD_CAP_DO,
	},
	{
		.cmd	= NLDLM_CMD_GET_NODE,
		.dumpit	= nldlm_nl_get_node_dumpit,
		.flags	= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= NLDLM_CMD_ADD_NODE,
		.doit		= nldlm_nl_add_node_doit,
		.policy		= nldlm_add_node_nl_policy,
		.maxattr	= NLDLM_A_NODE_ADDRS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_DEL_NODE,
		.doit		= nldlm_nl_del_node_doit,
		.policy		= nldlm_del_node_nl_policy,
		.maxattr	= NLDLM_A_NODE_ID,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_GET_LS,
		.doit		= nldlm_nl_get_ls_doit,
		.policy		= nldlm_get_ls_nl_policy,
		.maxattr	= NLDLM_A_LS_NAME,
		.flags		= GENL_CMD_CAP_DO,
	},
	{
		.cmd	= NLDLM_CMD_GET_LS,
		.dumpit	= nldlm_nl_get_ls_dumpit,
		.flags	= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= NLDLM_CMD_GET_LS_MEMBER,
		.doit		= nldlm_nl_get_ls_member_doit,
		.policy		= nldlm_get_ls_member_do_nl_policy,
		.maxattr	= NLDLM_A_LS_MEMBER_NODEID,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_GET_LS_MEMBER,
		.dumpit		= nldlm_nl_get_ls_member_dumpit,
		.policy		= nldlm_get_ls_member_dump_nl_policy,
		.maxattr	= NLDLM_A_LS_MEMBER_LS_NAME,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= NLDLM_CMD_LS_ADD_MEMBER,
		.doit		= nldlm_nl_ls_add_member_doit,
		.policy		= nldlm_ls_add_member_nl_policy,
		.maxattr	= NLDLM_A_LS_MEMBER_WEIGHT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_LS_DEL_MEMBER,
		.doit		= nldlm_nl_ls_del_member_doit,
		.policy		= nldlm_ls_del_member_nl_policy,
		.maxattr	= NLDLM_A_LS_MEMBER_NODEID,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_LS_CTRL,
		.doit		= nldlm_nl_ls_ctrl_doit,
		.policy		= nldlm_ls_ctrl_nl_policy,
		.maxattr	= NLDLM_A_LS_CTRL_ACTION,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_LS_EVENT_DONE,
		.doit		= nldlm_nl_ls_event_done_doit,
		.policy		= nldlm_ls_event_done_nl_policy,
		.maxattr	= NLDLM_A_LS_EVENT_RESULT_RESULT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd	= NLDLM_CMD_GET_CFG,
		.doit	= nldlm_nl_get_cfg_doit,
		.flags	= GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_SET_OUR_NODEID,
		.doit		= nldlm_nl_set_our_nodeid_doit,
		.policy		= nldlm_set_our_nodeid_nl_policy,
		.maxattr	= NLDLM_A_CFG_OUR_NODEID,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_SET_CLUSTER_NAME,
		.doit		= nldlm_nl_set_cluster_name_doit,
		.policy		= nldlm_set_cluster_name_nl_policy,
		.maxattr	= NLDLM_A_CFG_CLUSTER_NAME,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_SET_PROTOCOL,
		.doit		= nldlm_nl_set_protocol_doit,
		.policy		= nldlm_set_protocol_nl_policy,
		.maxattr	= NLDLM_A_CFG_PROTOCOL,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_SET_PORT,
		.doit		= nldlm_nl_set_port_doit,
		.policy		= nldlm_set_port_nl_policy,
		.maxattr	= NLDLM_A_CFG_PORT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_SET_RECOVER_TIMEOUT,
		.doit		= nldlm_nl_set_recover_timeout_doit,
		.policy		= nldlm_set_recover_timeout_nl_policy,
		.maxattr	= NLDLM_A_CFG_RECOVER_TIMEOUT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_SET_INACTIVE_TIMEOUT,
		.doit		= nldlm_nl_set_inactive_timeout_doit,
		.policy		= nldlm_set_inactive_timeout_nl_policy,
		.maxattr	= NLDLM_A_CFG_INACTIVE_TIMEOUT,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_SET_LOG_LEVEL,
		.doit		= nldlm_nl_set_log_level_doit,
		.policy		= nldlm_set_log_level_nl_policy,
		.maxattr	= NLDLM_A_CFG_LOG_LEVEL,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_SET_DEFAULT_MARK,
		.doit		= nldlm_nl_set_default_mark_doit,
		.policy		= nldlm_set_default_mark_nl_policy,
		.maxattr	= NLDLM_A_CFG_DEFAULT_MARK,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd		= NLDLM_CMD_SET_RECOVER_CALLBACKS,
		.doit		= nldlm_nl_set_recover_callbacks_doit,
		.policy		= nldlm_set_recover_callbacks_nl_policy,
		.maxattr	= NLDLM_A_CFG_RECOVER_CALLBACKS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
};

static const struct genl_multicast_group nldlm_nl_mcgrps[] = {
	[NLDLM_NLGRP_LS_EVENT] = { "ls-event", },
};

struct genl_family nldlm_nl_family __ro_after_init = {
	.name		= NLDLM_FAMILY_NAME,
	.version	= NLDLM_FAMILY_VERSION,
	.netnsok	= true,
	.parallel_ops	= true,
	.module		= THIS_MODULE,
	.split_ops	= nldlm_nl_ops,
	.n_split_ops	= ARRAY_SIZE(nldlm_nl_ops),
	.mcgrps		= nldlm_nl_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(nldlm_nl_mcgrps),
};
