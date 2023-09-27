// SPDX-License-Identifier: GPL-2.0
#include <linux/debugfs.h>
#include <linux/memory_hotplug.h>
#include <linux/seq_file.h>

#include <asm/ptdump.h>

static int ptdump_show(struct seq_file *m, void *v)
{
	struct ptdump_info *info = m->private;

	get_online_mems();
	if (info->ptdump_walk)
		info->ptdump_walk(m, info);
	put_online_mems();
	return 0;
}

static int ptdump_open(struct inode *inode, struct file *file)
{
	int ret;
	struct ptdump_info *info = inode->i_private;

	ret = single_open(file, ptdump_show, inode->i_private);
	if (!ret && info->ptdump_prepare_walk) {
		mutex_lock(&info->file_lock);
		info->ptdump_prepare_walk(info);
	}
	return ret;
}

static int ptdump_release(struct inode *inode, struct file *file)
{
	struct ptdump_info *info = inode->i_private;

	if (info->ptdump_end_walk) {
		info->ptdump_end_walk(info);
		mutex_unlock(&info->file_lock);
	}

	return single_release(inode, file);
}

static const struct file_operations ptdump_fops = {
	.owner		= THIS_MODULE,
	.open		= ptdump_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= ptdump_release,
};

void __init ptdump_debugfs_register(struct ptdump_info *info, const char *name)
{
	debugfs_create_file(name, 0400, NULL, info, &ptdump_fops);
}
