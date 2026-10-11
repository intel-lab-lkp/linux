// SPDX-License-Identifier: GPL-2.0
/*
 * Generic uprobe infrastructure for RV monitors.
 */
#include <linux/fs.h>
#include <linux/uprobes.h>
#include <rv/rv_uprobe.h>

/*
 * uprobe_register() takes no inode reference of its own, so grab one here and
 * drop it only after rv_uprobe_unregister_sync().
 */
int rv_uprobe_register(struct inode *inode, loff_t offset, struct rv_uprobe *p)
{
	int ret;

	p->inode = igrab(inode);
	if (!p->inode)
		return -ESTALE;

	p->uprobe = uprobe_register(inode, offset, 0, &p->uc);
	if (IS_ERR(p->uprobe)) {
		ret = PTR_ERR(p->uprobe);
		p->uprobe = NULL;
		iput(p->inode);
		p->inode = NULL;
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(rv_uprobe_register);

bool rv_uprobe_is_registered(const struct rv_uprobe *p)
{
	return p && p->uprobe;
}
EXPORT_SYMBOL_GPL(rv_uprobe_is_registered);

/* The inode stays held: in-flight handlers may still reach it. */
void rv_uprobe_unregister_nosync(struct rv_uprobe *p)
{
	if (!p || !p->uprobe)
		return;

	uprobe_unregister_nosync(p->uprobe, &p->uc);
	p->uprobe = NULL;
}
EXPORT_SYMBOL_GPL(rv_uprobe_unregister_nosync);

/* Global barrier over every probe dequeued so far. */
void rv_uprobe_unregister_sync(void)
{
	uprobe_unregister_sync();
}
EXPORT_SYMBOL_GPL(rv_uprobe_unregister_sync);
