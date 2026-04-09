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
#include "internal.h"

/* A global variable is a bit ugly, but it keeps the code simple */
static int sysctl_drop_caches;

static void drop_pagecache_sb(struct super_block *sb, void *node)
{
	struct inode *inode, *toput_inode = NULL;
	int nid = *(int *)node;

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

		invalidate_node_mapping_pages(inode->i_mapping, 0, -1, nid);
		iput(toput_inode);
		toput_inode = inode;

		cond_resched();
		spin_lock(&sb->s_inode_list_lock);
	}
	spin_unlock(&sb->s_inode_list_lock);
	iput(toput_inode);
}

static unsigned long has_caches(int nid)
{
	unsigned long nr;

	if (nid >= 0)
		nr = node_page_state(NODE_DATA(nid), NR_FILE_PAGES);
	else
		nr = global_node_page_state(NR_FILE_PAGES);

	return nr;
}

static void drop_caches_handler(int flags, int nid)
{
	static int stfu;

	if (flags & 1) {
		if (!has_caches(nid))
			return;

		lru_add_drain_all();
		iterate_supers(drop_pagecache_sb, &nid);
		count_vm_event(DROP_PAGECACHE);
	}

	if (flags & 2) {
		drop_slab(nid);
		count_vm_event(DROP_SLAB);
	}

	if (!stfu) {
		if (nid >= 0)
			pr_info("%s (%d): drop_caches: %d on node %d\n",
				current->comm, task_pid_nr(current), flags, nid);
		else
			pr_info("%s (%d): drop_caches: %d\n",
				current->comm, task_pid_nr(current), flags);
	}
	stfu |= flags & 4;
}

static int drop_caches_sysctl_handler(const struct ctl_table *table, int write,
		void *buffer, size_t *length, loff_t *ppos)
{
	int ret;

	ret = proc_dointvec_minmax(table, write, buffer, length, ppos);
	if (ret)
		return ret;
	if (write)
		drop_caches_handler(sysctl_drop_caches, NUMA_NO_NODE);
	return 0;
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
};

static int __init init_vm_drop_caches_sysctls(void)
{
	register_sysctl_init("vm", drop_caches_table);
	return 0;
}
fs_initcall(init_vm_drop_caches_sysctls);

#ifdef CONFIG_NUMA
/* The range of input is same as sysctl_drop_caches */
#define INPUT_MIN 1
#define INPUT_MAX 4
static ssize_t drop_caches_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int nid = dev->id;
	int input;

	if (kstrtoint(buf, 0, &input))
		return -EINVAL;

	if (input > INPUT_MAX || input < INPUT_MIN)
		return -EINVAL;

	if (nid >= 0 && nid < nr_node_ids && node_online(nid))
		drop_caches_handler(input, nid);

	return count;
}

static DEVICE_ATTR_WO(drop_caches);
int drop_caches_register_node(struct node *node)
{
	return device_create_file(&node->dev, &dev_attr_drop_caches);
}

void drop_caches_unregister_node(struct node *node)
{
	device_remove_file(&node->dev, &dev_attr_drop_caches);
}
#endif
