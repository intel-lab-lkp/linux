/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2026 Wen Yang <wen.yang@linux.dev> */
/*
 * Generic uprobe infrastructure for RV monitors.
 */

#ifndef _RV_UPROBE_H
#define _RV_UPROBE_H

#include <linux/types.h>
#include <linux/uprobes.h>

struct inode;
struct pt_regs;

/*
 * Free the containing struct only after rv_uprobe_unregister_nosync() +
 * rv_uprobe_unregister_sync() + iput(p->inode).
 *
 * The inode reference does not pin the dentry, so callers that re-resolve the
 * probed path must hold a struct path: 9p cache=none remints an inode per
 * lookup.
 */
struct rv_uprobe {
	struct uprobe_consumer	uc;
	struct uprobe		*uprobe;
	struct inode		*inode;
};

bool rv_uprobe_is_registered(const struct rv_uprobe *p);
int rv_uprobe_register(struct inode *inode, loff_t offset, struct rv_uprobe *p);
void rv_uprobe_unregister_nosync(struct rv_uprobe *p);
void rv_uprobe_unregister_sync(void);

#endif /* _RV_UPROBE_H */
