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
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/fs_context.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/slab.h>

#include <linux/configfs.h>
#include "configfs_internal.h"

/* Random magic number */
#define CONFIGFS_MAGIC 0x62656570

struct kmem_cache *configfs_dir_cachep;
static DEFINE_XARRAY(configfs_super_xa);
static struct configfs_super_info *configfs_root;
static DEFINE_SPINLOCK(configfs_pin_lock);

static u64 configfs_ns_id(struct net *net_ns)
{
	struct ns_common *ns = net_ns ? to_ns_common(net_ns) : NULL;

	if (!ns || net_ns == &init_net)
		return 0;
	return ns->ns_id;
}

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

bool configfs_is_root(struct config_item *item)
{
	struct configfs_dirent *sd;

	if (!item->ci_dentry)
		return false;
	sd = item->ci_dentry->d_fsdata;
	return !!(sd->s_type & CONFIGFS_ROOT);
}

static void configfs_fill_super_info(struct configfs_super_info *info)
{
	INIT_LIST_HEAD(&info->root.s_sibling);
	INIT_LIST_HEAD(&info->root.s_children);
	info->root.s_type = CONFIGFS_ROOT;
	info->root.s_element = &info->group.cg_item;
	strscpy(info->group.cg_item.ci_namebuf, "root", 4);
	info->group.cg_item.ci_name = info->group.cg_item.ci_namebuf;
	config_group_init(&info->group);
	INIT_LIST_HEAD(&info->subsys_list);
	mutex_init(&info->subsys_mutex);
	refcount_set(&info->ref, 1);
	info->mnt_count = 0;
}

struct configfs_super_info *configfs_get_super_info(struct net *net_ns)
{
	struct configfs_super_info *info;
	u64 ns_id = configfs_ns_id(net_ns);
	int err;

	xa_lock(&configfs_super_xa);
	info = xa_load(&configfs_super_xa, ns_id);
	if (info) {
		if (!refcount_inc_not_zero((&info->ref)))
			info = ERR_PTR(-EBUSY);
		xa_unlock(&configfs_super_xa);
		return info;
	}
	info = kzalloc_obj(*info);
	if (!info) {
		xa_unlock(&configfs_super_xa);
		return ERR_PTR(-ENOMEM);
	}

	info->net_ns = get_net(net_ns);
	configfs_fill_super_info(info);
	err = __xa_insert(&configfs_super_xa, ns_id,
			  info, GFP_KERNEL);
	xa_unlock(&configfs_super_xa);
	if (err < 0) {
		put_net(info->net_ns);
		kfree(info);
		return ERR_PTR(err);
	}
	return info;
}

void configfs_put_super_info(struct configfs_super_info *info)
{
	u64 ns_id = configfs_ns_id(info->net_ns);

	xa_lock(&configfs_super_xa);
	if (!refcount_dec_and_test(&info->ref)) {
		xa_unlock(&configfs_super_xa);
		return;
	}
	__xa_erase(&configfs_super_xa, ns_id);
	xa_unlock(&configfs_super_xa);
	put_net(info->net_ns);
	kfree(info);
}

static int configfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct configfs_super_info *info = sb->s_fs_info;
	struct inode *inode;
	struct dentry *root;

	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_magic = CONFIGFS_MAGIC;
	sb->s_op = &configfs_ops;
	sb->s_time_gran = 1;

	inode = configfs_new_inode(S_IFDIR | S_IRWXU | S_IRUGO | S_IXUGO,
				   &info->root, sb);
	if (IS_ERR(inode)) {
		pr_debug("could not get root inode\n");
		return PTR_ERR(inode);
	}
	inode->i_op = &configfs_root_inode_operations;
	inode->i_fop = &configfs_dir_operations;
	/* directory inodes start off with i_nlink == 2 (for "." entry) */
	inc_nlink(inode);

	root = d_make_root(inode);
	if (!root) {
		pr_debug("%s: could not get root dentry!\n",__func__);
		return -ENOMEM;
	}
	info->group.cg_item.ci_dentry = root;
	root->d_fsdata = &info->root;
	sb->s_root = root;
	set_default_d_op(sb, &configfs_dentry_ops); /* the rest get that */
	sb->s_d_flags |= DCACHE_DONTCACHE;

	configfs_link_subsystems(sb, info);

	return 0;
}

static int configfs_get_tree(struct fs_context *fc)
{
	struct net *net_ns = fc->fs_private;
	struct configfs_super_info *info;
	int err;

	info = configfs_get_super_info(net_ns);
	if (IS_ERR(info))
		return PTR_ERR(info);

	err = get_tree_keyed(fc, configfs_fill_super, info);
	if (err && fc->s_fs_info)
		configfs_put_super_info(info);
	return err;
}

static void configfs_fs_context_free(struct fs_context *fc)
{
	struct net *net_ns = fc->fs_private;

	fc->fs_private = NULL;
	put_net(net_ns);
}

static const struct fs_context_operations configfs_context_ops = {
	.get_tree	= configfs_get_tree,
	.free		= configfs_fs_context_free,
};

static int configfs_init_fs_context(struct fs_context *fc)
{
	struct net *net_ns = get_net(current->nsproxy->net_ns);

	fc->fs_private = net_ns;
	fc->ops = &configfs_context_ops;
	return 0;
}

static void configfs_kill_sb(struct super_block *sb)
{
	struct configfs_super_info *info = sb->s_fs_info;

	configfs_unlink_subsystems(sb, info);
	kill_anon_super(sb);
	configfs_put_super_info(info);
	sb->s_fs_info = NULL;
}

static struct file_system_type configfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "configfs",
	.init_fs_context = configfs_init_fs_context,
	.kill_sb	= configfs_kill_sb,
	.fs_flags	= FS_USERNS_MOUNT,
};
MODULE_ALIAS_FS("configfs");

struct dentry *configfs_pin_fs(struct super_block *sb)
{
	struct configfs_super_info *info;

	spin_lock(&configfs_pin_lock);
	if (sb) {
		struct configfs_super_info *root = configfs_root;
		struct dentry *dentry = sb->s_root;

		info = sb->s_fs_info;
		if (!info->mnt) {
			struct vfsmount *mnt;

			spin_unlock(&configfs_pin_lock);
			mnt = mnt_clone_direct(root->mnt, dentry);
			if (IS_ERR(mnt))
				return ERR_CAST(mnt);
			spin_lock(&configfs_pin_lock);
			info->mnt = mnt;
		} else {
			mntget(info->mnt);
		}
	} else {
		info = configfs_root;
		if (!info->mnt) {
			struct vfsmount *mnt;

			spin_unlock(&configfs_pin_lock);
			mnt = vfs_kern_mount(&configfs_fs_type, SB_KERNMOUNT,
					     configfs_fs_type.name, NULL);
			if (IS_ERR(mnt))
				return ERR_CAST(mnt);
			spin_lock(&configfs_pin_lock);
			info->mnt = mnt;
		} else {
			mntget(info->mnt);
		}
	}
	info->mnt_count++;
	spin_unlock(&configfs_pin_lock);
	return info->mnt->mnt_root;
}

void configfs_release_fs(struct super_block *sb)
{
	struct configfs_super_info *info = configfs_root;
	struct vfsmount *mnt;

	spin_lock(&configfs_pin_lock);
	if (sb)
		info = sb->s_fs_info;
	mnt = info->mnt;
	if (--info->mnt_count)
		info->mnt = NULL;
	spin_unlock(&configfs_pin_lock);
	mntput(mnt);
}

static int __init configfs_init(void)
{
	int err = -ENOMEM;

	configfs_dir_cachep = kmem_cache_create("configfs_dir_cache",
						sizeof(struct configfs_dirent),
						0, 0, NULL);
	if (!configfs_dir_cachep)
		goto out;

	configfs_root = configfs_get_super_info(&init_net);
	if (IS_ERR(configfs_root)) {
		err = PTR_ERR(configfs_root);
		goto out2;
	}

	err = sysfs_create_mount_point(kernel_kobj, "config");
	if (err)
		goto out3;

	err = register_filesystem(&configfs_fs_type);
	if (err)
		goto out4;

	return 0;
out4:
	pr_err("Unable to register filesystem!\n");
	sysfs_remove_mount_point(kernel_kobj, "config");
out3:
	configfs_put_super_info(configfs_root);
	configfs_root = NULL;
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
	configfs_put_super_info(configfs_root);
	configfs_root = NULL;
	kmem_cache_destroy(configfs_dir_cachep);
	configfs_dir_cachep = NULL;
}

MODULE_AUTHOR("Oracle");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.0.2");
MODULE_DESCRIPTION("Simple RAM filesystem for user driven kernel subsystem configuration.");

core_initcall(configfs_init);
module_exit(configfs_exit);
