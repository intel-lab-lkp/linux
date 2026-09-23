// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mount.c - operations for initializing and mounting configfs.
 *
 * Based on sysfs:
 * 	sysfs is Copyright (C) 2001, 2002, 2003 Patrick Mochel
 *
 * configfs Copyright (C) 2005 Oracle.  All rights reserved.
 */

#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/fs_context.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/slab.h>

#include <linux/configfs.h>
#include "configfs_internal.h"

static struct vfsmount *configfs_mount = NULL;
struct kmem_cache *configfs_dir_cachep;
static int configfs_mnt_count = 0;


static void configfs_free_inode(struct inode *inode)
{
	if (S_ISLNK(inode->i_mode))
		kfree(inode->i_link);
	free_inode_nonrcu(inode);
}

static const struct super_operations configfs_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= inode_just_drop,
	.free_inode	= configfs_free_inode,
};

static struct config_group configfs_root_group = {
	.cg_item = {
		.ci_namebuf	= "root",
		.ci_name	= configfs_root_group.cg_item.ci_namebuf,
	},
};

int configfs_is_root(struct config_item *item)
{
	return item == &configfs_root_group.cg_item;
}

static struct configfs_dirent configfs_root = {
	.s_sibling	= LIST_HEAD_INIT(configfs_root.s_sibling),
	.s_children	= LIST_HEAD_INIT(configfs_root.s_children),
	.s_element	= &configfs_root_group.cg_item,
	.s_type		= CONFIGFS_ROOT,
	.s_iattr	= NULL,
};

static int configfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *inode;
	struct dentry *root;

	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_magic = CONFIGFS_MAGIC;
	sb->s_op = &configfs_ops;
	sb->s_time_gran = 1;

	inode = configfs_new_inode(S_IFDIR | S_IRWXU | S_IRUGO | S_IXUGO,
				   &configfs_root, sb);
	if (inode) {
		inode->i_op = &configfs_root_inode_operations;
		inode->i_fop = &configfs_dir_operations;
		/* directory inodes start off with i_nlink == 2 (for "." entry) */
		inc_nlink(inode);
	} else {
		pr_debug("could not get root inode\n");
		return -ENOMEM;
	}

	root = d_make_root(inode);
	if (!root) {
		pr_debug("%s: could not get root dentry!\n",__func__);
		return -ENOMEM;
	}
	config_group_init(&configfs_root_group);
	configfs_root_group.cg_item.ci_dentry = root;
	root->d_fsdata = &configfs_root;
	sb->s_root = root;
	set_default_d_op(sb, &configfs_dentry_ops); /* the rest get that */
	sb->s_d_flags |= DCACHE_DONTCACHE;
	return 0;
}

static int configfs_get_tree(struct fs_context *fc)
{
	return get_tree_single(fc, configfs_fill_super);
}

static const struct fs_context_operations configfs_context_ops = {
	.get_tree	= configfs_get_tree,
};

static int configfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &configfs_context_ops;
	return 0;
}

static struct file_system_type configfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "configfs",
	.init_fs_context = configfs_init_fs_context,
	.kill_sb	= kill_anon_super,
};
MODULE_ALIAS_FS("configfs");

/**
 * configfs_open_root - open a path relative to an already-resolved root
 * @root: resolved root, which must not be on configfs
 * @name: path to open relative to @root, or "" to open @root itself
 * @flags: open flags as per the open(2) second argument
 * @mode: mode argument passed to file_open_root()
 *
 * Open @name relative to @root, refusing a @root on configfs.
 *
 * Configfs store callbacks are called with the fragment semaphore of the
 * item they belong to held for reading.  Opening a configfs path from
 * such a callback can re-enter __configfs_open_file() and take that same
 * semaphore again, which is not recursive and deadlocks.  Callers that
 * open a user-configured path from a configfs store callback must
 * therefore use this helper instead of filp_open() or a bare
 * file_open_root().
 *
 * Resolving @name relative to @root also lets a caller pin and validate a
 * root once and keep opening files below it, rather than re-resolving a
 * pathname that can be retargeted in the meantime.  The normal open-time
 * permission and security checks still apply to the file being opened.
 *
 * Return: the opened file, or an ERR_PTR() value.  -EINVAL is returned if
 * @root is on configfs.
 */
struct file *configfs_open_root(const struct path *root, const char *name,
				int flags, umode_t mode)
{
	if (root->dentry->d_sb->s_type == &configfs_fs_type)
		return ERR_PTR(-EINVAL);

	return file_open_root(root, name, flags, mode);
}
EXPORT_SYMBOL_GPL(configfs_open_root);

/**
 * configfs_file_open - open a pathname that must not resolve to configfs
 * @filename: existing pathname to resolve and open
 * @flags: open flags as per the open(2) second argument
 * @mode: mode argument passed to file_open_root()
 *
 * Resolve @filename and open the resulting file, refusing to open it if it
 * resolves to configfs.  @filename must already exist; this helper cannot
 * create a missing path.  Use this from configfs store callbacks that open
 * a path configured by the user, in place of filp_open().  See
 * configfs_open_root() for why opening configfs again from such a callback
 * deadlocks.
 *
 * Return: the opened file, or an ERR_PTR() value.  -EINVAL is returned if
 * @filename resolves to configfs.
 */
struct file *configfs_file_open(const char *filename, int flags, umode_t mode)
{
	struct file *file;
	struct path path;
	int ret;

	ret = kern_path(filename, LOOKUP_FOLLOW, &path);
	if (ret)
		return ERR_PTR(ret);

	file = configfs_open_root(&path, "", flags, mode);
	path_put(&path);
	return file;
}
EXPORT_SYMBOL_GPL(configfs_file_open);

struct dentry *configfs_pin_fs(void)
{
	int err = simple_pin_fs(&configfs_fs_type, &configfs_mount,
			     &configfs_mnt_count);
	return err ? ERR_PTR(err) : configfs_mount->mnt_root;
}

void configfs_release_fs(void)
{
	simple_release_fs(&configfs_mount, &configfs_mnt_count);
}


static int __init configfs_init(void)
{
	int err = -ENOMEM;

	configfs_dir_cachep = kmem_cache_create("configfs_dir_cache",
						sizeof(struct configfs_dirent),
						0, 0, NULL);
	if (!configfs_dir_cachep)
		goto out;

	err = sysfs_create_mount_point(kernel_kobj, "config");
	if (err)
		goto out2;

	err = register_filesystem(&configfs_fs_type);
	if (err)
		goto out3;

	return 0;
out3:
	pr_err("Unable to register filesystem!\n");
	sysfs_remove_mount_point(kernel_kobj, "config");
out2:
	kmem_cache_destroy(configfs_dir_cachep);
	configfs_dir_cachep = NULL;
out:
	return err;
}

static void __exit configfs_exit(void)
{
	unregister_filesystem(&configfs_fs_type);
	sysfs_remove_mount_point(kernel_kobj, "config");
	kmem_cache_destroy(configfs_dir_cachep);
	configfs_dir_cachep = NULL;
}

MODULE_AUTHOR("Oracle");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.0.2");
MODULE_DESCRIPTION("Simple RAM filesystem for user driven kernel subsystem configuration.");

core_initcall(configfs_init);
module_exit(configfs_exit);
