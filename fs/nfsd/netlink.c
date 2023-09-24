// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/nfsd.yaml */
/* YNL-GEN kernel source */

#include <net/netlink.h>
#include <net/genetlink.h>

#include "netlink.h"

#include <uapi/linux/nfsd_netlink.h>

/* NFSD_CMD_THREADS_SET - do */
static const struct nla_policy nfsd_threads_set_nl_policy[NFSD_A_SERVER_ATTR_THREADS + 1] = {
	[NFSD_A_SERVER_ATTR_THREADS] = { .type = NLA_U32, },
};

/* NFSD_CMD_MAX_BLKSIZE_SET - do */
static const struct nla_policy nfsd_max_blksize_set_nl_policy[NFSD_A_SERVER_ATTR_MAX_BLKSIZE + 1] = {
	[NFSD_A_SERVER_ATTR_MAX_BLKSIZE] = { .type = NLA_U32, },
};

/* NFSD_CMD_MAX_CONN_SET - do */
static const struct nla_policy nfsd_max_conn_set_nl_policy[NFSD_A_SERVER_ATTR_MAX_CONN + 1] = {
	[NFSD_A_SERVER_ATTR_MAX_CONN] = { .type = NLA_U32, },
};

/* Ops table for nfsd */
static const struct genl_split_ops nfsd_nl_ops[] = {
	{
		.cmd	= NFSD_CMD_RPC_STATUS_GET,
		.start	= nfsd_nl_rpc_status_get_start,
		.dumpit	= nfsd_nl_rpc_status_get_dumpit,
		.done	= nfsd_nl_rpc_status_get_done,
		.flags	= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= NFSD_CMD_THREADS_SET,
		.doit		= nfsd_nl_threads_set_doit,
		.policy		= nfsd_threads_set_nl_policy,
		.maxattr	= NFSD_A_SERVER_ATTR_THREADS,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd	= NFSD_CMD_THREADS_GET,
		.dumpit	= nfsd_nl_threads_get_dumpit,
		.flags	= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= NFSD_CMD_MAX_BLKSIZE_SET,
		.doit		= nfsd_nl_max_blksize_set_doit,
		.policy		= nfsd_max_blksize_set_nl_policy,
		.maxattr	= NFSD_A_SERVER_ATTR_MAX_BLKSIZE,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd	= NFSD_CMD_MAX_BLKSIZE_GET,
		.dumpit	= nfsd_nl_max_blksize_get_dumpit,
		.flags	= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= NFSD_CMD_MAX_CONN_SET,
		.doit		= nfsd_nl_max_conn_set_doit,
		.policy		= nfsd_max_conn_set_nl_policy,
		.maxattr	= NFSD_A_SERVER_ATTR_MAX_CONN,
		.flags		= GENL_ADMIN_PERM | GENL_CMD_CAP_DO,
	},
	{
		.cmd	= NFSD_CMD_MAX_CONN_GET,
		.dumpit	= nfsd_nl_max_conn_get_dumpit,
		.flags	= GENL_CMD_CAP_DUMP,
	},
};

struct genl_family nfsd_nl_family __ro_after_init = {
	.name		= NFSD_FAMILY_NAME,
	.version	= NFSD_FAMILY_VERSION,
	.netnsok	= true,
	.parallel_ops	= true,
	.module		= THIS_MODULE,
	.split_ops	= nfsd_nl_ops,
	.n_split_ops	= ARRAY_SIZE(nfsd_nl_ops),
};
