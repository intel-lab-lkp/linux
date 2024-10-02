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
int sysctl_drop_caches;

static int invalidate_inode_fn(struct inode *inode, void *data)
{
	if (!mapping_empty(inode->i_mapping))
		invalidate_mapping_pages(inode->i_mapping, 0, -1);
	return INO_ITER_DONE;
}

/*
 * Note: it would be nice to check mapping_empty() before we get a reference on
 * the inode in super_iter_inodes(), but that's a future optimisation.
 */
static void drop_pagecache_sb(struct super_block *sb, void *unused)
{
	super_iter_inodes(sb, invalidate_inode_fn, NULL, 0);
}

int drop_caches_sysctl_handler(const struct ctl_table *table, int write,
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
