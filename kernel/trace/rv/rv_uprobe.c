// SPDX-License-Identifier: GPL-2.0
/*
 * Generic uprobe infrastructure for RV monitors.
 *
 * struct rv_uprobe embeds struct uprobe_consumer directly.  This is safe
 * because rv_uprobe_sync() calls uprobe_unregister_sync(), which calls
 * synchronize_rcu_tasks_trace().  handler_chain() runs under
 * rcu_read_lock_trace(), so after synchronize_rcu_tasks_trace() returns,
 * all in-flight handler_chain() iterations, including any pending
 * uc->cons_node.next reads, have completed on all CPUs.  The caller may
 * then free the struct containing rv_uprobe immediately.
 */
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/uprobes.h>
#include <rv/rv_uprobe.h>

/**
 * rv_uprobe_register - initialise and register an uprobe
 */
int rv_uprobe_register(const char *binpath, loff_t offset, struct rv_uprobe *p)
{
	struct inode *inode;
	struct path path;
	int ret;

	if (!p->uc.handler && !p->uc.ret_handler)
		return -EINVAL;

	ret = kern_path(binpath, LOOKUP_FOLLOW, &path);
	if (ret)
		return ret;

	if (!d_is_reg(path.dentry)) {
		path_put(&path);
		return -EINVAL;
	}

	inode = d_real_inode(path.dentry);
	p->inode = inode;

	/*
	 * uprobe_register() requires the inode (and mount) to remain
	 * referenced across the call.  Keep the path alive until after
	 * uprobe_register() has stored its own reference, then release it.
	 */
	p->uprobe = uprobe_register(inode, offset, 0, &p->uc);
	path_put(&path);
	if (IS_ERR(p->uprobe)) {
		ret = PTR_ERR(p->uprobe);
		p->uprobe = NULL;
		p->inode = NULL;
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(rv_uprobe_register);

/**
 * rv_uprobe_is_registered - test whether an uprobe is currently active
 */
bool rv_uprobe_is_registered(const struct rv_uprobe *p)
{
	return p && p->uprobe;
}
EXPORT_SYMBOL_GPL(rv_uprobe_is_registered);

/**
 * rv_uprobe_unregister - synchronously unregister a uprobe
 */
void rv_uprobe_unregister(struct rv_uprobe *p)
{
	if (!p || !p->uprobe)
		return;

	rv_uprobe_unregister_nosync(p);
	rv_uprobe_sync();
}
EXPORT_SYMBOL_GPL(rv_uprobe_unregister);

/**
 * rv_uprobe_unregister_nosync - dequeue an uprobe without waiting
 */
void rv_uprobe_unregister_nosync(struct rv_uprobe *p)
{
	if (!p || !p->uprobe)
		return;

	uprobe_unregister_nosync(p->uprobe, &p->uc);
	p->uprobe = NULL;
	p->inode = NULL;
}
EXPORT_SYMBOL_GPL(rv_uprobe_unregister_nosync);

/**
 * rv_uprobe_sync - wait for all in-flight uprobe handlers to complete
 */
void rv_uprobe_sync(void)
{
	uprobe_unregister_sync();
}
EXPORT_SYMBOL_GPL(rv_uprobe_sync);
