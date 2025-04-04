// SPDX-License-Identifier: GPL-2.0-only
/*
 * Enable cooperating processes to share page table between
 * them to reduce the extra memory consumed by multiple copies
 * of page tables.
 *
 * This code adds an in-memory filesystem - msharefs.
 * msharefs is used to manage page table sharing
 *
 *
 * Copyright (C) 2024 Oracle Corp. All rights reserved.
 * Author:	Khalid Aziz <khalid@kernel.org>
 *
 */

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <uapi/linux/magic.h>

const unsigned long mshare_align = P4D_SIZE;

static const struct file_operations msharefs_file_operations = {
	.open			= simple_open,
};

struct msharefs_info {
	struct dentry *info_dentry;
};

static ssize_t
mshare_info_read(struct file *file, char __user *buf, size_t nbytes,
		loff_t *ppos)
{
	char s[80];

	sprintf(s, "%ld\n", mshare_align);
	return simple_read_from_buffer(buf, nbytes, ppos, s, strlen(s));
}

static const struct file_operations mshare_info_ops = {
	.read	= mshare_info_read,
	.llseek	= noop_llseek,
};

static const struct super_operations mshare_s_ops = {
	.statfs		= simple_statfs,
};

static int
msharefs_create_mshare_info(struct super_block *sb)
{
	struct msharefs_info *info = sb->s_fs_info;
	struct dentry *root = sb->s_root;
	struct dentry *dentry;
	struct inode *inode;
	int ret;

	ret = -ENOMEM;
	inode = new_inode(sb);
	if (!inode)
		goto out;

	inode->i_ino = 2;
	simple_inode_init_ts(inode);
	inode_init_owner(&nop_mnt_idmap, inode, NULL, S_IFREG | 0444);
	inode->i_fop = &mshare_info_ops;

	dentry = d_alloc_name(root, "mshare_info");
	if (!dentry)
		goto out;

	info->info_dentry = dentry;
	d_add(dentry, inode);

	return 0;
out:
	iput(inode);

	return ret;
}

static int
msharefs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct msharefs_info *info;
	struct inode *inode;
	int ret;

	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_magic		= MSHARE_MAGIC;
	sb->s_op		= &mshare_s_ops;
	sb->s_time_gran		= 1;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	sb->s_fs_info = info;

	inode = new_inode(sb);
	if (!inode)
		return -ENOMEM;

	inode->i_ino = 1;
	inode->i_mode = S_IFDIR | 0777;
	simple_inode_init_ts(inode);
	inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;
	set_nlink(inode, 2);

	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	ret = msharefs_create_mshare_info(sb);

	return ret;
}

static int
msharefs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, msharefs_fill_super);
}

static const struct fs_context_operations msharefs_context_ops = {
	.get_tree	= msharefs_get_tree,
};

static int
mshare_init_fs_context(struct fs_context *fc)
{
	fc->ops = &msharefs_context_ops;
	return 0;
}

static void
msharefs_kill_super(struct super_block *sb)
{
	struct msharefs_info *info = sb->s_fs_info;

	kfree(info);
	kill_litter_super(sb);
}

static struct file_system_type mshare_fs = {
	.name			= "msharefs",
	.init_fs_context	= mshare_init_fs_context,
	.kill_sb		= msharefs_kill_super,
};

static int __init
mshare_init(void)
{
	int ret;

	ret = sysfs_create_mount_point(fs_kobj, "mshare");
	if (ret)
		return ret;

	ret = register_filesystem(&mshare_fs);
	if (ret)
		sysfs_remove_mount_point(fs_kobj, "mshare");

	return ret;
}

core_initcall(mshare_init);
