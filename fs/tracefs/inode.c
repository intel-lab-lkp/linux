// SPDX-License-Identifier: GPL-2.0-only
/*
 *  inode.c - part of tracefs, a pseudo file system for activating tracing
 *
 * Based on debugfs by: Greg Kroah-Hartman <greg@kroah.com>
 *
 *  Copyright (C) 2014 Red Hat Inc, author: Steven Rostedt <srostedt@redhat.com>
 *
 * tracefs is the file system that is used by the tracing infrastructure.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_parser.h>
#include <linux/mount.h>
#include <linux/kobject.h>
#include <linux/namei.h>
#include <linux/tracefs.h>
#include <linux/fsnotify.h>
#include <linux/security.h>
#include <linux/seq_file.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include "internal.h"

#define TRACEFS_DEFAULT_MODE	0700
static struct kernfs_root *trace_fs_root;
static struct kernfs_node *trace_kfs_root_node;

static struct vfsmount *tracefs_mount;
static int tracefs_mount_count;
static bool tracefs_registered;

static ssize_t trace_fs_kf_read(struct kernfs_open_file *of, char *buf,
				size_t count, loff_t pos)
{
	return 0;
}

static ssize_t trace_fs_kf_write(struct kernfs_open_file *of, char *buf,
				 size_t count, loff_t pos)
{
	return 0;
}

static loff_t trace_fs_kf_llseek(struct kernfs_open_file *of, loff_t offset,
				 int whence)
{
	return noop_llseek(of->file, offset, whence);
}

static int trace_fs_kf_open(struct kernfs_open_file *of)
{
	return 0;
}

static const struct kernfs_ops tracefs_file_kfops = {
	.read		= trace_fs_kf_read,
	.write		= trace_fs_kf_write,
	.open		= trace_fs_kf_open,
	.llseek		= trace_fs_kf_llseek,
};

static struct tracefs_dir_ops {
	int (*mkdir)(const char *name);
	int (*rmdir)(const char *name);
} tracefs_ops __ro_after_init;

struct inode *tracefs_get_inode(struct super_block *sb)
{
	struct inode *inode = new_inode(sb);
	if (inode) {
		inode->i_ino = get_next_ino();
		simple_inode_init_ts(inode);
	}
	return inode;
}

struct tracefs_mount_opts {
	kuid_t uid;
	kgid_t gid;
	umode_t mode;
	/* Opt_* bitfield. */
	unsigned int opts;
};

struct tracefs_mount_opts global_opts = {
	.mode	= TRACEFS_DEFAULT_MODE,
	.uid	= GLOBAL_ROOT_UID,
	.gid	= GLOBAL_ROOT_GID,
	.opts	= 0,
};

enum trace_fs_param {
	Opt_uid,
	Opt_gid,
	Opt_mode,
};

static const struct fs_parameter_spec trace_fs_parameters[] = {
	fsparam_u32   ("gid",		Opt_gid),
	fsparam_u32oct("mode",		Opt_mode),
	fsparam_u32   ("uid",		Opt_uid),
	{}
};

struct trace_fs_context {
	struct kernfs_fs_context kfc;
	struct tracefs_mount_opts mount_opts;
};

static inline struct trace_fs_context *trace_fc2context(struct fs_context *fc)
{
	struct kernfs_fs_context *kfc = fc->fs_private;

	return container_of(kfc, struct trace_fs_context, kfc);
}

static int trace_fs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct trace_fs_context *ctx = trace_fc2context(fc);
	struct tracefs_mount_opts *mount_opts = &ctx->mount_opts;
	struct fs_parse_result result;
	int opt;
	kuid_t kuid;
	kgid_t kgid;

	opt = fs_parse(fc, trace_fs_parameters, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_mode:
		mount_opts->mode = result.uint_32 & 07777;
		mount_opts->opts |= BIT(Opt_mode);
		break;
	case Opt_uid:
		kuid = make_kuid(current_user_ns(), result.uint_32);
		if (!uid_valid(kuid))
			goto bad_value;

		/*
		 * The requested uid must be representable in the
		 * filesystem's idmapping.
		 */
		if (!kuid_has_mapping(fc->user_ns, kuid))
			goto bad_value;

		mount_opts->uid = kuid;
		mount_opts->opts |= BIT(Opt_uid);
		break;
	case Opt_gid:
		kgid = make_kgid(current_user_ns(), result.uint_32);
		if (!gid_valid(kgid))
			goto bad_value;

		/*
		 * The requested gid must be representable in the
		 * filesystem's idmapping.
		 */
		if (!kgid_has_mapping(fc->user_ns, kgid))
			goto bad_value;

		mount_opts->gid = kgid;
		mount_opts->opts |= BIT(Opt_gid);
		break;
	default:
		return invalfc(fc, "Unsupported parameter '%s'", param->key);
	}

bad_value:
	return invalfc(fc, "Bad value for '%s'", param->key);
}

static int tracefs_apply_options(struct super_block *sb, bool remount)
{
	struct inode *inode = d_inode(sb->s_root);
	kuid_t kuid = global_opts.uid;
	kgid_t kgid = global_opts.gid;
	umode_t mode = global_opts.mode;
	unsigned int opts = global_opts.opts;
	umode_t tmp_mode;

	/*
	 * On remount, only reset mode/uid/gid if they were provided as mount
	 * options.
	 */

	if (!remount || opts & BIT(Opt_mode)) {
		tmp_mode = READ_ONCE(inode->i_mode) & ~S_IALLUGO;
		tmp_mode |= mode;
		WRITE_ONCE(inode->i_mode, tmp_mode);
	}

	if (!remount || opts & BIT(Opt_uid))
		inode->i_uid = kuid;

	if (!remount || opts & BIT(Opt_gid))
		inode->i_gid = kgid;

	return 0;
}

static int trace_fs_reconfigure(struct fs_context *fc)
{
	tracefs_apply_options(fc->root->d_sb, true);
	return 0;
}

static int trace_fs_show_options(struct seq_file *seq, struct kernfs_root *kf_root)
{
	kuid_t kuid = global_opts.uid;
	kgid_t kgid = global_opts.gid;
	umode_t mode = global_opts.mode;

	if (!uid_eq(kuid, GLOBAL_ROOT_UID))
		seq_printf(seq, ",uid=%u", from_kuid_munged(&init_user_ns, kuid));
	if (!gid_eq(kgid, GLOBAL_ROOT_GID))
		seq_printf(seq, ",gid=%u", from_kgid_munged(&init_user_ns, kgid));
	if (mode != TRACEFS_DEFAULT_MODE)
		seq_printf(seq, ",mode=%o", mode);

	return 0;
}

static int trace_fs_mkdir(struct kernfs_node *parent_kn, const char *name, umode_t mode)
{
	int ret;
	struct kernfs_node *kn;

	if (parent_kn != trace_instance_dir)
		return -EPERM;

	kn = tracefs_create_dir(name, parent_kn);
	if (IS_ERR(kn))
		return PTR_ERR(kn);

	ret = tracefs_ops.mkdir(name);
	if (ret)
		kernfs_remove(kn);
	return ret;
}

static int trace_fs_rmdir(struct kernfs_node *kn)
{
	int ret;

	if (kn != trace_instance_dir)
		return -EPERM;

 	ret = tracefs_ops.rmdir(kn->name);
	if (!ret)
		kernfs_remove(kn);

	return ret;
}

static struct kernfs_syscall_ops trace_fs_kf_syscall_ops = {
	.show_options		= trace_fs_show_options,
	.mkdir			= trace_fs_mkdir,
	.rmdir			= trace_fs_rmdir,
};

static int trace_fs_get_tree(struct fs_context *fc)
{
	int ret;

	ret = kernfs_get_tree(fc);
	if (!ret)
		tracefs_apply_options(fc->root->d_sb, false);
	return ret;
}

static void trace_fs_context_free(struct fs_context *fc)
{
	struct trace_fs_context *ctx = trace_fc2context(fc);
	kernfs_free_fs_context(fc);
	kfree(ctx);
}

static const struct fs_context_operations trace_fs_context_ops = {
	.free		= trace_fs_context_free,
	.parse_param	= trace_fs_parse_param,
	.get_tree	= trace_fs_get_tree,
	.reconfigure	= trace_fs_reconfigure,
};

static int trace_fs_init_fs_context(struct fs_context *fc)
{
	struct trace_fs_context *ctx;

	ctx = kzalloc(sizeof(struct trace_fs_context), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->kfc.magic = TRACEFS_MAGIC;
	ctx->mount_opts.mode = TRACEFS_DEFAULT_MODE;
	fc->fs_private = &ctx->kfc;
	fc->global = true;
	fc->ops = &trace_fs_context_ops;
	return 0;
}

static struct file_system_type trace_fs_type = {
	.name			= "tracefs",
	.init_fs_context	= trace_fs_init_fs_context,
	.parameters		= trace_fs_parameters,
	.kill_sb		= kill_litter_super,
};
MODULE_ALIAS_FS("tracefs");

struct dentry *tracefs_start_creating(const char *name, struct dentry *parent)
{
	struct dentry *dentry;
	int error;

	pr_debug("tracefs: creating file '%s'\n",name);

	error = simple_pin_fs(&trace_fs_type, &tracefs_mount,
			      &tracefs_mount_count);
	if (error)
		return ERR_PTR(error);

	/* If the parent is not specified, we create it in the root.
	 * We need the root dentry to do this, which is in the super
	 * block. A pointer to that is in the struct vfsmount that we
	 * have around.
	 */
	if (!parent)
		parent = tracefs_mount->mnt_root;

	inode_lock(d_inode(parent));
	if (unlikely(IS_DEADDIR(d_inode(parent))))
		dentry = ERR_PTR(-ENOENT);
	else
		dentry = lookup_one_len(name, parent, strlen(name));
	if (!IS_ERR(dentry) && d_inode(dentry)) {
		dput(dentry);
		dentry = ERR_PTR(-EEXIST);
	}

	if (IS_ERR(dentry)) {
		inode_unlock(d_inode(parent));
		simple_release_fs(&tracefs_mount, &tracefs_mount_count);
	}

	return dentry;
}

struct dentry *tracefs_failed_creating(struct dentry *dentry)
{
	inode_unlock(d_inode(dentry->d_parent));
	dput(dentry);
	simple_release_fs(&tracefs_mount, &tracefs_mount_count);
	return NULL;
}

struct dentry *tracefs_end_creating(struct dentry *dentry)
{
	inode_unlock(d_inode(dentry->d_parent));
	return dentry;
}

/**
 * eventfs_start_creating - start the process of creating a dentry
 * @name: Name of the file created for the dentry
 * @parent: The parent dentry where this dentry will be created
 *
 * This is a simple helper function for the dynamically created eventfs
 * files. When the directory of the eventfs files are accessed, their
 * dentries are created on the fly. This function is used to start that
 * process.
 */
struct dentry *eventfs_start_creating(const char *name, struct dentry *parent)
{
	struct dentry *dentry;
	int error;

	/* Must always have a parent. */
	if (WARN_ON_ONCE(!parent))
		return ERR_PTR(-EINVAL);

	error = simple_pin_fs(&trace_fs_type, &tracefs_mount,
			      &tracefs_mount_count);
	if (error)
		return ERR_PTR(error);

	if (unlikely(IS_DEADDIR(parent->d_inode)))
		dentry = ERR_PTR(-ENOENT);
	else
		dentry = lookup_one_len(name, parent, strlen(name));

	if (!IS_ERR(dentry) && dentry->d_inode) {
		dput(dentry);
		dentry = ERR_PTR(-EEXIST);
	}

	if (IS_ERR(dentry))
		simple_release_fs(&tracefs_mount, &tracefs_mount_count);

	return dentry;
}

/**
 * eventfs_failed_creating - clean up a failed eventfs dentry creation
 * @dentry: The dentry to clean up
 *
 * If after calling eventfs_start_creating(), a failure is detected, the
 * resources created by eventfs_start_creating() needs to be cleaned up. In
 * that case, this function should be called to perform that clean up.
 */
struct dentry *eventfs_failed_creating(struct dentry *dentry)
{
	dput(dentry);
	simple_release_fs(&tracefs_mount, &tracefs_mount_count);
	return NULL;
}

/**
 * eventfs_end_creating - Finish the process of creating a eventfs dentry
 * @dentry: The dentry that has successfully been created.
 *
 * This function is currently just a place holder to match
 * eventfs_start_creating(). In case any synchronization needs to be added,
 * this function will be used to implement that without having to modify
 * the callers of eventfs_start_creating().
 */
struct dentry *eventfs_end_creating(struct dentry *dentry)
{
	return dentry;
}

/**
 * tracefs_create_file - create a file in the tracefs filesystem
 * @name: a pointer to a string containing the name of the file to create.
 * @mode: the permission that the file should have.
 * @parent: a pointer to the parent dentry for this file.  This should be a
 *          directory dentry if set.  If this parameter is NULL, then the
 *          file will be created in the root of the tracefs filesystem.
 * @data: a pointer to something that the caller will want to get to later
 *        on.  The inode.i_private pointer will point to this value on
 *        the open() call.
 * @fops: a pointer to a struct file_operations that should be used for
 *        this file.
 *
 * This is the basic "create a file" function for tracefs.  It allows for a
 * wide range of flexibility in creating a file, or a directory (if you want
 * to create a directory, the tracefs_create_dir() function is
 * recommended to be used instead.)
 *
 * This function will return a pointer to a dentry if it succeeds.  This
 * pointer must be passed to the tracefs_remove() function when the file is
 * to be removed (no automatic cleanup happens if your module is unloaded,
 * you are responsible here.)  If an error occurs, %NULL will be returned.
 *
 * If tracefs is not enabled in the kernel, the value -%ENODEV will be
 * returned.
 */
struct kernfs_node *tracefs_create_file(const char *name, umode_t mode,
					struct kernfs_node *parent, void *data,
					const struct kernfs_ops *ops)
{
	if (security_locked_down(LOCKDOWN_TRACEFS))
		return NULL;

	if (!(mode & S_IFMT))
		mode |= S_IFREG;
	BUG_ON(!S_ISREG(mode));

	// inode->i_op = &tracefs_file_inode_operations;

	return __kernfs_create_file(parent ?: trace_kfs_root_node, name, mode,
				    kernfs_node_owner(parent),
				    kernfs_node_group(parent), PAGE_SIZE,
				    ops ? : &tracefs_file_kfops, data, NULL,
				    NULL);
}

/**
 * tracefs_create_dir - create a directory in the tracefs filesystem
 * @name: a pointer to a string containing the name of the directory to
 *        create.
 * @parent: a pointer to the parent dentry for this file.  This should be a
 *          directory dentry if set.  If this parameter is NULL, then the
 *          directory will be created in the root of the tracefs filesystem.
 *
 * This function creates a directory in tracefs with the given name.
 *
 * This function will return a pointer to a dentry if it succeeds.  This
 * pointer must be passed to the tracefs_remove() function when the file is
 * to be removed. If an error occurs, %NULL will be returned.
 *
 * If tracing is not enabled in the kernel, the value -%ENODEV will be
 * returned.
 */
struct kernfs_node *tracefs_create_dir(const char *name,
				       struct kernfs_node *parent)
{
	if (security_locked_down(LOCKDOWN_TRACEFS))
		return ERR_PTR(-EINVAL);

	return kernfs_create_dir_ns(parent ?: trace_kfs_root_node, name,
				  S_IFDIR | S_IRWXU | S_IRUSR | S_IRGRP |
				  S_IXUSR | S_IXGRP,
				  kernfs_node_owner(parent),
				  kernfs_node_group(parent), NULL, NULL);
}

/**
 * tracefs_create_instance_dir - create the tracing instances directory
 * @name: The name of the instances directory to create
 * @parent: The parent directory that the instances directory will exist
 * @mkdir: The function to call when a mkdir is performed.
 * @rmdir: The function to call when a rmdir is performed.
 *
 * Only one instances directory is allowed.
 *
 * The instances directory is special as it allows for mkdir and rmdir
 * to be done by userspace. When a mkdir or rmdir is performed, the inode
 * locks are released and the methods passed in (@mkdir and @rmdir) are
 * called without locks and with the name of the directory being created
 * within the instances directory.
 *
 * Returns the dentry of the instances directory.
 */
__init struct kernfs_node *
tracefs_create_instance_dir(int (*mkdir)(const char *name),
			    int (*rmdir)(const char *name))
{
	struct kernfs_node *kn;

	/* Only allow one instance of the instances directory. */
	if (WARN_ON(tracefs_ops.mkdir || tracefs_ops.rmdir))
		return ERR_PTR(-EINVAL);

	kn = tracefs_create_dir("instances", trace_kfs_root_node);
	if (IS_ERR(kn))
		return kn;

	tracefs_ops.mkdir = mkdir;
	tracefs_ops.rmdir = rmdir;
	return kn;
}

/**
 * tracefs_remove - recursively removes a directory
 * @dentry: a pointer to a the dentry of the directory to be removed.
 *
 * This function recursively removes a directory tree in tracefs that
 * was previously created with a call to another tracefs function
 * (like tracefs_create_file() or variants thereof.)
 */
void tracefs_remove(struct kernfs_node *kn)
{
	if (IS_ERR_OR_NULL(kn))
		return;

	kernfs_remove(kn);
}

/**
 * tracefs_initialized - Tells whether tracefs has been registered
 */
bool tracefs_initialized(void)
{
	return tracefs_registered;
}

static int __init tracefs_init(void)
{
	int retval;
	struct kernfs_root *kfs_root;

	kfs_root = kernfs_create_root(&trace_fs_kf_syscall_ops,
				      KERNFS_ROOT_CREATE_DEACTIVATED, NULL);
	if (IS_ERR(kfs_root))
                return PTR_ERR(kfs_root);

	retval = sysfs_create_mount_point(kernel_kobj, "tracing");
	if (retval) {
		kernfs_destroy_root(kfs_root);
		return -EINVAL;
	}

	retval = register_filesystem(&trace_fs_type);
	if (!retval)
		tracefs_registered = true;
	else
		kernfs_destroy_root(kfs_root);

	trace_fs_root = kfs_root;
	trace_kfs_root_node = kernfs_root_to_node(kfs_root);

	return retval;
}
core_initcall(tracefs_init);
