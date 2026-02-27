// SPDX-License-Identifier: GPL-2.0
/*
 * Implement the manual drop-all-pagecache function
 */

#include <linux/pagemap.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/writeback.h>
#include <linux/sysctl.h>
#include <linux/gfp.h>
#include <linux/swap.h>
#include <linux/task_work.h>
#include <linux/namei.h>
#include "internal.h"

/* A global variable is a bit ugly, but it keeps the code simple */
static int sysctl_drop_caches;

static void drop_pagecache_sb(struct super_block *sb, void *unused)
{
	struct inode *inode, *toput_inode = NULL;

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		spin_lock(&inode->i_lock);
		/*
		 * We must skip inodes in unusual state. We may also skip
		 * inodes without pages but we deliberately won't in case
		 * we need to reschedule to avoid softlockups.
		 */
		if ((inode_state_read(inode) & (I_FREEING | I_WILL_FREE | I_NEW)) ||
		    (mapping_empty(inode->i_mapping) && !need_resched())) {
			spin_unlock(&inode->i_lock);
			continue;
		}
		__iget(inode);
		spin_unlock(&inode->i_lock);
		spin_unlock(&sb->s_inode_list_lock);

		invalidate_mapping_pages(inode->i_mapping, 0, -1);
		iput(toput_inode);
		toput_inode = inode;

		cond_resched();
		spin_lock(&sb->s_inode_list_lock);
	}
	spin_unlock(&sb->s_inode_list_lock);
	iput(toput_inode);
}

static int drop_caches_sysctl_handler(const struct ctl_table *table, int write,
		void *buffer, size_t *length, loff_t *ppos)
{
	int ret;

	ret = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (ret)
		return ret;
	if (write) {
		static int stfu;

		if (sysctl_drop_caches & 1) {
			lru_add_drain_all();
			iterate_supers(drop_pagecache_sb, NULL);
			count_vm_event(DROP_PAGECACHE);
		}
		if (sysctl_drop_caches & 2) {
			drop_slab();
			count_vm_event(DROP_SLAB);
		}
		if (!stfu) {
			pr_info("%s (%d): drop_caches: %d\n",
				current->comm, task_pid_nr(current),
				sysctl_drop_caches);
		}
		stfu |= sysctl_drop_caches & 4;
	}
	return 0;
}

struct drop_fs_caches_work {
	struct callback_head task_work;
	dev_t dev;
	char *path;
	unsigned int ctl;
};

static void drop_fs_caches(struct callback_head *twork)
{
	int ret;
	struct super_block *sb;
	static bool suppress;
	struct drop_fs_caches_work *work = container_of(twork,
			struct drop_fs_caches_work, task_work);
	unsigned int ctl = work->ctl;
	dev_t dev = work->dev;

	if (work->path) {
		struct path path;

		ret = kern_path(work->path, LOOKUP_FOLLOW, &path);
		if (ret) {
			pr_err("%s (%d): %s: failed to get path(%s) %d\n",
			       current->comm, task_pid_nr(current),
			       __func__, work->path, ret);
			goto out;
		}
		dev = path.dentry->d_sb->s_dev;
		/* Make this file's dentry and inode recyclable */
		path_put(&path);
	}

	sb = user_get_super(dev, false);
	if (!sb) {
		pr_err("%s (%d): %s: failed to get dev(%u:%u)'s sb\n",
		       current->comm, task_pid_nr(current), __func__,
		       MAJOR(dev), MINOR(dev));
		goto out;
	}

	if (ctl & BIT(0)) {
		lru_add_drain_all();
		drop_pagecache_sb(sb, NULL);
		count_vm_event(DROP_PAGECACHE);
	}

	if (ctl & BIT(1)) {
		drop_sb_dentry_inode(sb);
		count_vm_event(DROP_SLAB);
	}

	if (!READ_ONCE(suppress)) {
		pr_info("%s (%d): %s: %d %u:%u\n", current->comm,
			task_pid_nr(current), __func__, ctl,
			MAJOR(sb->s_dev), MINOR(sb->s_dev));

		if (ctl & BIT(2))
			WRITE_ONCE(suppress, true);
	}

	drop_super(sb);
out:
	kfree(work->path);
	kfree(work);
}

static int drop_fs_caches_sysctl_handler(const struct ctl_table *table,
					 int write, void *buffer,
					 size_t *length, loff_t *ppos)
{
	struct drop_fs_caches_work *work = NULL;
	unsigned int major, minor;
	unsigned int ctl;
	int ret;
	char *path = NULL;

	if (!write)
		return 0;

	if (sscanf(buffer, "%u %u:%u", &ctl, &major, &minor) != 3) {
		path = kstrdup(buffer, GFP_NOFS);
		if (!path) {
			ret = -ENOMEM;
			goto out;
		}

		if (sscanf(buffer, "%u %s", &ctl, path) != 2) {
			ret = -EINVAL;
			goto out;
		}
	}

	if (ctl < 1 || ctl > 7) {
		ret = -EINVAL;
		goto out;
	}

	work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (!work) {
		ret = -ENOMEM;
		goto out;
	}

	init_task_work(&work->task_work, drop_fs_caches);
	if (!path)
		work->dev = MKDEV(major, minor);
	work->path = path;
	work->ctl = ctl;
	ret = task_work_add(current, &work->task_work, TWA_RESUME);
out:
	if (ret) {
		kfree(path);
		kfree(work);
	}

	return ret;
}

static const struct ctl_table drop_caches_table[] = {
	{
		.procname	= "drop_caches",
		.data		= &sysctl_drop_caches,
		.maxlen		= sizeof(int),
		.mode		= 0200,
		.proc_handler	= drop_caches_sysctl_handler,
		.extra1		= SYSCTL_ONE,
		.extra2		= SYSCTL_FOUR,
	},
	{
		.procname	= "drop_fs_caches",
		.mode		= 0200,
		.proc_handler	= drop_fs_caches_sysctl_handler,
	},
};

static int __init init_vm_drop_caches_sysctls(void)
{
	register_sysctl_init("vm", drop_caches_table);
	return 0;
}
fs_initcall(init_vm_drop_caches_sysctls);
