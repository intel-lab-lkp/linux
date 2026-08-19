// SPDX-License-Identifier: GPL-2.0
/*
 * Generic uprobe infrastructure for RV monitors.
 *
 * rv_uprobe embeds struct uprobe_consumer; rv_uprobe_sync() drains in-flight
 * handlers before the containing struct may be freed (see rv_uprobe.h).
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
	int ret;

	ret = kern_path(binpath, LOOKUP_FOLLOW, &p->path);
	if (ret)
		return ret;

	if (!d_is_reg(p->path.dentry)) {
		path_put(&p->path);
		return -EINVAL;
	}

	inode = d_real_inode(p->path.dentry);

	/* uprobe_register() takes no inode reference; the path is held in p->path */
	p->uprobe = uprobe_register(inode, offset, 0, &p->uc);
	if (IS_ERR(p->uprobe)) {
		ret = PTR_ERR(p->uprobe);
		p->uprobe = NULL;
		path_put(&p->path);
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

	uprobe_unregister_nosync(p->uprobe, &p->uc);
	p->uprobe = NULL;
	rv_uprobe_sync();
	path_put(&p->path);
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
	/* path held; caller must call rv_uprobe_sync() then path_put(&p->path) */
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
