// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Copyright (C) 2007
 *
 *  Author: Eric Biederman <ebiederm@xmision.com>
 */

#include <linux/module.h>
#include <linux/ipc.h>
#include <linux/nsproxy.h>
#include <linux/sysctl.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include <linux/ipc_namespace.h>
#include <linux/msg.h>
#include <linux/cred.h>
#include "util.h"

static int proc_ipc_dointvec_minmax_orphans(const struct ctl_table *table, int write,
		void *buffer, size_t *lenp, loff_t *ppos)
{
	struct ipc_namespace *ns =
		container_of(table->data, struct ipc_namespace, shm_rmid_forced);
	int err;

	err = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (err < 0)
		return err;
	if (write && ns->shm_rmid_forced)
		shm_destroy_orphaned(ns);
	return err;
}

static int proc_ipc_auto_msgmni(const struct ctl_table *table, int write,
		void *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table ipc_table;
	int dummy = 0;

	memcpy(&ipc_table, table, sizeof(ipc_table));
	ipc_table.data = &dummy;
	ipc_table.extra1 = SYSCTL_ZERO;
	ipc_table.extra2 = SYSCTL_ONE;

	if (write)
		pr_info_once("writing to auto_msgmni has no effect");

	return proc_dointvec_minmax(&ipc_table, write, buffer, lenp, ppos);
}

static int proc_ipc_sem_dointvec(const struct ctl_table *table, int write,
	void *buffer, size_t *lenp, loff_t *ppos)
{
	struct ipc_namespace *ns =
		container_of(table->data, struct ipc_namespace, sem_ctls);
	int ret, semmni;

	semmni = ns->sem_ctls[3];
	ret = proc_dointvec(table, write, buffer, lenp, ppos);

	if (!ret)
		ret = sem_check_semmni(ns);

	/*
	 * Reset the semmni value if an error happens.
	 */
	if (ret)
		ns->sem_ctls[3] = semmni;
	return ret;
}

int ipc_mni = IPCMNI;
int ipc_mni_shift = IPCMNI_SHIFT;
int ipc_min_cycle = RADIX_TREE_MAP_SIZE;
static unsigned int ipc_mni_max = IPCMNI;
static unsigned int ipc_uint_max = INT_MAX;

static const struct sysctl_field ipc_sysctls[] = {
	{
		.procname	= "shmmax",
		.mode		= 0644,
		.type		= SYSCTL_FIELD_SIZE_T,
		.data_offset	= SYSCTL_FIELD_SIZE_T_OFFSET(struct ipc_namespace,
							     shm_ctlmax),
	},
	{
		.procname	= "shmall",
		.mode		= 0644,
		.type		= SYSCTL_FIELD_SIZE_T,
		.data_offset	= SYSCTL_FIELD_SIZE_T_OFFSET(struct ipc_namespace,
							     shm_ctlall),
	},
	{
		.procname	= "shmmni",
		.mode		= 0644,
		.type		= SYSCTL_FIELD_INT_MINMAX,
		.data_offset	= SYSCTL_FIELD_INT_OFFSET(struct ipc_namespace,
							  shm_ctlmni),
		.int_limits = {
			.min	= SYSCTL_ZERO,
			.max	= &ipc_mni,
		},
	},
	{
		.procname	= "shm_rmid_forced",
		.mode		= 0644,
		.proc_handler	= proc_ipc_dointvec_minmax_orphans,
		.type		= SYSCTL_FIELD_INT_MINMAX,
		.data_offset	= SYSCTL_FIELD_INT_OFFSET(struct ipc_namespace,
							  shm_rmid_forced),
		.int_limits = {
			.min	= SYSCTL_ZERO,
			.max	= SYSCTL_ONE,
		},
	},
	{
		.procname	= "msgmax",
		.mode		= 0644,
		.type		= SYSCTL_FIELD_UINT_MINMAX,
		.data_offset	= SYSCTL_FIELD_UINT_OFFSET(struct ipc_namespace,
							   msg_ctlmax),
		.uint_limits = {
			.min	= SYSCTL_UINT_ZERO,
			.max	= &ipc_uint_max,
		},
	},
	{
		.procname	= "msgmni",
		.mode		= 0644,
		.type		= SYSCTL_FIELD_UINT_MINMAX,
		.data_offset	= SYSCTL_FIELD_UINT_OFFSET(struct ipc_namespace,
							   msg_ctlmni),
		.uint_limits = {
			.min	= SYSCTL_UINT_ZERO,
			.max	= &ipc_mni_max,
		},
	},
	{
		.procname	= "auto_msgmni",
		.mode		= 0644,
		.proc_handler	= proc_ipc_auto_msgmni,
		.maxlen		= sizeof(int),
		.type		= SYSCTL_FIELD_NO_DATA,
	},
	{
		.procname	= "msgmnb",
		.mode		= 0644,
		.type		= SYSCTL_FIELD_UINT_MINMAX,
		.data_offset	= SYSCTL_FIELD_UINT_OFFSET(struct ipc_namespace,
							   msg_ctlmnb),
		.uint_limits = {
			.min	= SYSCTL_UINT_ZERO,
			.max	= &ipc_uint_max,
		},
	},
	{
		.procname	= "sem",
		.mode		= 0644,
		.proc_handler	= proc_ipc_sem_dointvec,
		.maxlen		= 4 * sizeof(int),
		.type		= SYSCTL_FIELD_INT,
		.data_offset	= SYSCTL_FIELD_INT_OFFSET(struct ipc_namespace,
							  sem_ctls[0]),
	},
#ifdef CONFIG_CHECKPOINT_RESTORE
	{
		.procname	= "sem_next_id",
		.mode		= 0444,
		.type		= SYSCTL_FIELD_INT_MINMAX,
		.data_offset	= SYSCTL_FIELD_INT_OFFSET(struct ipc_namespace,
							  ids[IPC_SEM_IDS].next_id),
		.int_limits = {
			.min	= SYSCTL_ZERO,
			.max	= SYSCTL_INT_MAX,
		},
	},
	{
		.procname	= "msg_next_id",
		.mode		= 0444,
		.type		= SYSCTL_FIELD_INT_MINMAX,
		.data_offset	= SYSCTL_FIELD_INT_OFFSET(struct ipc_namespace,
							  ids[IPC_MSG_IDS].next_id),
		.int_limits = {
			.min	= SYSCTL_ZERO,
			.max	= SYSCTL_INT_MAX,
		},
	},
	{
		.procname	= "shm_next_id",
		.mode		= 0444,
		.type		= SYSCTL_FIELD_INT_MINMAX,
		.data_offset	= SYSCTL_FIELD_INT_OFFSET(struct ipc_namespace,
							  ids[IPC_SHM_IDS].next_id),
		.int_limits = {
			.min	= SYSCTL_ZERO,
			.max	= SYSCTL_INT_MAX,
		},
	},
#endif
};

static struct ctl_table_set *set_lookup(struct ctl_table_root *root)
{
	return &current->nsproxy->ipc_ns->ipc_set;
}

static int set_is_seen(struct ctl_table_set *set)
{
	return &current->nsproxy->ipc_ns->ipc_set == set;
}

static void ipc_set_ownership(struct ctl_table_header *head,
			      kuid_t *uid, kgid_t *gid)
{
	struct ipc_namespace *ns =
		container_of(head->set, struct ipc_namespace, ipc_set);

	kuid_t ns_root_uid = make_kuid(ns->user_ns, 0);
	kgid_t ns_root_gid = make_kgid(ns->user_ns, 0);

	*uid = uid_valid(ns_root_uid) ? ns_root_uid : GLOBAL_ROOT_UID;
	*gid = gid_valid(ns_root_gid) ? ns_root_gid : GLOBAL_ROOT_GID;
}

static int ipc_permissions(struct ctl_table_header *head, const struct ctl_table *table)
{
	int mode = table->mode;

#ifdef CONFIG_CHECKPOINT_RESTORE
	struct ipc_namespace *ns =
		container_of(head->set, struct ipc_namespace, ipc_set);

	if (((table->data == &ns->ids[IPC_SEM_IDS].next_id) ||
	     (table->data == &ns->ids[IPC_MSG_IDS].next_id) ||
	     (table->data == &ns->ids[IPC_SHM_IDS].next_id)) &&
	    checkpoint_restore_ns_capable_noaudit(ns->user_ns))
		mode = 0666;
	else
#endif
	{
		kuid_t ns_root_uid;
		kgid_t ns_root_gid;

		ipc_set_ownership(head, &ns_root_uid, &ns_root_gid);

		if (uid_eq(current_euid(), ns_root_uid))
			mode >>= 6;

		else if (in_egroup_p(ns_root_gid))
			mode >>= 3;
	}

	mode &= 7;

	return (mode << 6) | (mode << 3) | mode;
}

static struct ctl_table_root set_root = {
	.lookup = set_lookup,
	.permissions = ipc_permissions,
	.set_ownership = ipc_set_ownership,
};

bool setup_ipc_sysctls(struct ipc_namespace *ns)
{
	struct sysctl_context ctx = {
		.type = SYSCTL_CONTEXT_IPC_NS,
		.object_size = sizeof(*ns),
		.ns.ipc_ns = ns,
	};

	setup_sysctl_set(&ns->ipc_set, &set_root, set_is_seen);
	ns->ipc_sysctls = register_sysctl_fields(&ns->ipc_set, "kernel",
						 ipc_sysctls, &ctx);
	if (!ns->ipc_sysctls) {
		retire_sysctl_set(&ns->ipc_set);
		return false;
	}

	return true;
}

void retire_ipc_sysctls(struct ipc_namespace *ns)
{
	unregister_sysctl_table(ns->ipc_sysctls);
	retire_sysctl_set(&ns->ipc_set);
}

static int __init ipc_sysctl_init(void)
{
	if (!setup_ipc_sysctls(&init_ipc_ns)) {
		pr_warn("ipc sysctl registration failed\n");
		return -ENOMEM;
	}
	return 0;
}

device_initcall(ipc_sysctl_init);

static int __init ipc_mni_extend(char *str)
{
	ipc_mni = IPCMNI_EXTEND;
	ipc_mni_max = IPCMNI_EXTEND;
	ipc_mni_shift = IPCMNI_EXTEND_SHIFT;
	ipc_min_cycle = IPCMNI_EXTEND_MIN_CYCLE;
	pr_info("IPCMNI extended to %d.\n", ipc_mni);
	return 0;
}
early_param("ipcmni_extend", ipc_mni_extend);
