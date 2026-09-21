// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 2007 IBM Corporation
 *
 *  Author: Cedric Le Goater <clg@fr.ibm.com>
 */

#include <linux/nsproxy.h>
#include <linux/ipc_namespace.h>
#include <linux/sysctl.h>

#include <linux/stat.h>
#include <linux/capability.h>
#include <linux/slab.h>
#include <linux/cred.h>

static unsigned int msg_max_limit_min = MIN_MSGMAX;
static unsigned int msg_max_limit_max = HARD_MSGMAX;

static unsigned int msg_maxsize_limit_min = MIN_MSGSIZEMAX;
static unsigned int msg_maxsize_limit_max = HARD_MSGSIZEMAX;

static const struct sysctl_field mq_sysctls[] = {
	{
		.procname	= "queues_max",
		.mode		= 0644,
		.type		= SYSCTL_FIELD_UINT,
		.data_offset	= SYSCTL_FIELD_UINT_OFFSET(struct ipc_namespace,
							   mq_queues_max),
	},
	{
		.procname	= "msg_max",
		.mode		= 0644,
		.type		= SYSCTL_FIELD_UINT_MINMAX,
		.data_offset	= SYSCTL_FIELD_UINT_OFFSET(struct ipc_namespace,
							   mq_msg_max),
		.uint_limits = {
			.min	= &msg_max_limit_min,
			.max	= &msg_max_limit_max,
		},
	},
	{
		.procname	= "msgsize_max",
		.mode		= 0644,
		.type		= SYSCTL_FIELD_UINT_MINMAX,
		.data_offset	= SYSCTL_FIELD_UINT_OFFSET(struct ipc_namespace,
							   mq_msgsize_max),
		.uint_limits = {
			.min	= &msg_maxsize_limit_min,
			.max	= &msg_maxsize_limit_max,
		},
	},
	{
		.procname	= "msg_default",
		.mode		= 0644,
		.type		= SYSCTL_FIELD_UINT_MINMAX,
		.data_offset	= SYSCTL_FIELD_UINT_OFFSET(struct ipc_namespace,
							   mq_msg_default),
		.uint_limits = {
			.min	= &msg_max_limit_min,
			.max	= &msg_max_limit_max,
		},
	},
	{
		.procname	= "msgsize_default",
		.mode		= 0644,
		.type		= SYSCTL_FIELD_UINT_MINMAX,
		.data_offset	= SYSCTL_FIELD_UINT_OFFSET(struct ipc_namespace,
							   mq_msgsize_default),
		.uint_limits = {
			.min	= &msg_maxsize_limit_min,
			.max	= &msg_maxsize_limit_max,
		},
	},
};

static struct ctl_table_set *set_lookup(struct ctl_table_root *root)
{
	return &current->nsproxy->ipc_ns->mq_set;
}

static int set_is_seen(struct ctl_table_set *set)
{
	return &current->nsproxy->ipc_ns->mq_set == set;
}

static void mq_set_ownership(struct ctl_table_header *head,
			     kuid_t *uid, kgid_t *gid)
{
	struct ipc_namespace *ns =
		container_of(head->set, struct ipc_namespace, mq_set);

	kuid_t ns_root_uid = make_kuid(ns->user_ns, 0);
	kgid_t ns_root_gid = make_kgid(ns->user_ns, 0);

	*uid = uid_valid(ns_root_uid) ? ns_root_uid : GLOBAL_ROOT_UID;
	*gid = gid_valid(ns_root_gid) ? ns_root_gid : GLOBAL_ROOT_GID;
}

static int mq_permissions(struct ctl_table_header *head, const struct ctl_table *table)
{
	int mode = table->mode;
	kuid_t ns_root_uid;
	kgid_t ns_root_gid;

	mq_set_ownership(head, &ns_root_uid, &ns_root_gid);

	if (uid_eq(current_euid(), ns_root_uid))
		mode >>= 6;

	else if (in_egroup_p(ns_root_gid))
		mode >>= 3;

	mode &= 7;

	return (mode << 6) | (mode << 3) | mode;
}

static struct ctl_table_root set_root = {
	.lookup = set_lookup,
	.permissions = mq_permissions,
	.set_ownership = mq_set_ownership,
};

bool setup_mq_sysctls(struct ipc_namespace *ns)
{
	struct sysctl_context ctx = {
		.type = SYSCTL_CONTEXT_IPC_NS,
		.object_size = sizeof(*ns),
		.ns.ipc_ns = ns,
	};

	setup_sysctl_set(&ns->mq_set, &set_root, set_is_seen);

	ns->mq_sysctls = register_sysctl_fields(&ns->mq_set, "fs/mqueue",
						mq_sysctls, &ctx);
	if (!ns->mq_sysctls) {
		retire_sysctl_set(&ns->mq_set);
		return false;
	}

	return true;
}

void retire_mq_sysctls(struct ipc_namespace *ns)
{
	unregister_sysctl_table(ns->mq_sysctls);
	retire_sysctl_set(&ns->mq_set);
}
