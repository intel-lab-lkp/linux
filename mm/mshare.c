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

static const struct file_operations msharefs_file_operations = {
	.open			= simple_open,
};

static const struct super_operations mshare_s_ops = {
	.statfs		= simple_statfs,
};

static int
msharefs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *inode;

	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_magic		= MSHARE_MAGIC;
	sb->s_op		= &mshare_s_ops;
	sb->s_time_gran		= 1;

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

	return 0;
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

static struct file_system_type mshare_fs = {
	.name			= "msharefs",
	.init_fs_context	= mshare_init_fs_context,
	.kill_sb		= kill_litter_super,
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
