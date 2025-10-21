// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ns_common.h>
#include <linux/proc_ns.h>
#include <linux/user_namespace.h>
#include <linux/vfsdebug.h>

#ifdef CONFIG_DEBUG_VFS
static void ns_debug(struct ns_common *ns, const struct proc_ns_operations *ops)
{
	switch (ns->ns_type) {
#ifdef CONFIG_CGROUPS
	case CLONE_NEWCGROUP:
		VFS_WARN_ON_ONCE(ops != &cgroupns_operations);
		break;
#endif
#ifdef CONFIG_IPC_NS
	case CLONE_NEWIPC:
		VFS_WARN_ON_ONCE(ops != &ipcns_operations);
		break;
#endif
	case CLONE_NEWNS:
		VFS_WARN_ON_ONCE(ops != &mntns_operations);
		break;
#ifdef CONFIG_NET_NS
	case CLONE_NEWNET:
		VFS_WARN_ON_ONCE(ops != &netns_operations);
		break;
#endif
#ifdef CONFIG_PID_NS
	case CLONE_NEWPID:
		VFS_WARN_ON_ONCE(ops != &pidns_operations);
		break;
#endif
#ifdef CONFIG_TIME_NS
	case CLONE_NEWTIME:
		VFS_WARN_ON_ONCE(ops != &timens_operations);
		break;
#endif
#ifdef CONFIG_USER_NS
	case CLONE_NEWUSER:
		VFS_WARN_ON_ONCE(ops != &userns_operations);
		break;
#endif
#ifdef CONFIG_UTS_NS
	case CLONE_NEWUTS:
		VFS_WARN_ON_ONCE(ops != &utsns_operations);
		break;
#endif
	}
}
#endif

int __ns_common_init(struct ns_common *ns, u32 ns_type, const struct proc_ns_operations *ops, int inum)
{
	int ret;

	refcount_set(&ns->__ns_ref, 1);
	ns->stashed = NULL;
	ns->ops = ops;
	ns->ns_id = 0;
	ns->ns_type = ns_type;
	RB_CLEAR_NODE(&ns->ns_tree_node);
	INIT_LIST_HEAD(&ns->ns_list_node);

#ifdef CONFIG_DEBUG_VFS
	ns_debug(ns, ops);
#endif

	if (inum) {
		ns->inum = inum;
		return 0;
	}
	ret = proc_alloc_inum(&ns->inum);
	if (ret)
		return ret;
	/*
	 * Tree ref starts at 0. It's incremented when namespace enters
	 * active use (installed in nsproxy) and decremented when all
	 * active uses are gone. Initial namespaces are always active.
	 */
	if (is_initial_namespace(ns))
		atomic_set(&ns->__ns_ref_active, 1);
	else
		atomic_set(&ns->__ns_ref_active, 0);
	return 0;
}

void __ns_common_free(struct ns_common *ns)
{
	proc_free_inum(ns->inum);
}

void __ns_ref_active_get_owner(struct ns_common *ns)
{
	struct user_namespace *owner;

	if (unlikely(!ns->ops))
		return;
	VFS_WARN_ON_ONCE(!ns->ops->owner);
	owner = ns->ops->owner(ns);
	VFS_WARN_ON_ONCE(!owner && ns != to_ns_common(&init_user_ns));
	if (!owner)
		return;
	/* Skip init_user_ns as it's always active */
	if (owner == &init_user_ns)
		return;
	WARN_ON_ONCE(atomic_add_negative(1, &to_ns_common(owner)->__ns_ref_active));
}

void __ns_ref_active_put_owner(struct ns_common *ns)
{
	struct user_namespace *owner;

	do {
		if (unlikely(!ns->ops))
			return;
		VFS_WARN_ON_ONCE(!ns->ops->owner);
		owner = ns->ops->owner(ns);
		VFS_WARN_ON_ONCE(!owner && ns != to_ns_common(&init_user_ns));
		if (!owner)
			return;
		/* Skip init_user_ns as it's always active */
		if (owner == &init_user_ns)
			return;
		ns = to_ns_common(owner);
	} while (atomic_dec_and_test(&to_ns_common(owner)->__ns_ref_active));
}
