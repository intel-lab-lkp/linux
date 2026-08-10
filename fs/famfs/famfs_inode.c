// SPDX-License-Identifier: GPL-2.0
/*
 * famfs - dax file system for shared fabric-attached memory
 *
 * Copyright 2023-2024 Micron Technology, inc
 *
 * This file system, originally based on ramfs the dax support from xfs,
 * is intended to allow multiple host systems to mount a common file system
 * view of dax files that map to shared memory.
 */

#include <linux/fs.h>
#include <linux/cleanup.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/dax.h>
#include <linux/hugetlb.h>
#include <linux/iomap.h>
#include <linux/path.h>
#include <linux/namei.h>

#include "famfs_internal.h"

#define FAMFS_DEFAULT_MODE	0755

static struct inode *
famfs_get_inode(
	struct super_block *sb,
	const struct inode *dir,
	umode_t mode, dev_t dev)
{
	struct inode *inode = new_inode(sb);
	struct timespec64 tv;

	if (!inode)
		return NULL;

	inode->i_ino = get_next_ino();
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_mapping->a_ops = &ram_aops;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_unevictable(inode->i_mapping);
	tv = inode_set_ctime_current(inode);
	inode_set_mtime_to_ts(inode, tv);
	inode_set_atime_to_ts(inode, tv);

	switch (mode & S_IFMT) {
	default:
		init_special_inode(inode, mode, dev);
		break;
	case S_IFREG:
		inode->i_op = NULL /* famfs_file_inode_operations */;
		inode->i_fop = NULL /* &famfs_file_operations */;
		break;
	case S_IFDIR:
		inode->i_op = NULL /* famfs_dir_inode_operations */;
		inode->i_fop = &simple_dir_operations;

		/* Directory inodes start off with i_nlink == 2 (for ".") */
		inc_nlink(inode);
		break;
	case S_IFLNK:
		inode->i_op = &page_symlink_inode_operations;
		inode_nohighmem(inode);
		break;
	}
	return inode;
}

/*
 * famfs dax_operations (for famfs-mode dax)
 */
static void famfs_set_daxdev_err(struct famfs_fs_info *fsi,
				 struct dax_device *dax_devp);

static int
famfs_dax_notify_failure(
	struct dax_device *dax_dev,
	u64 offset,
	u64 len,
	int mf_flags)
{
	struct super_block *sb = dax_holder(dax_dev);
	struct famfs_fs_info *fsi = sb->s_fs_info;

	pr_err("%s: offset=%lld len=%llu flags=%x\n", __func__,
	       offset, len, mf_flags);

	/*
	 * Record the error on the specific daxdev and, near-term, shut the
	 * mount down: famfs_set_daxdev_err() also sets fsi->deverror so
	 * subsequent famfs operations fail. The resolver's per-daxdev
	 * famfs_dax_err() check remains and can make this finer later.
	 */
	famfs_set_daxdev_err(fsi, dax_dev);

	return 0;
}

static const struct dax_holder_operations famfs_dax_holder_ops = {
	.notify_failure		= famfs_dax_notify_failure,
};

/*
 * Allocate the daxdev table on first use (idempotent via cmpxchg).
 */
int
famfs_devlist_alloc(struct famfs_fs_info *fsi)
{
	struct famfs_dax_devlist *devlist;

	if (fsi->dax_devlist)
		return 0;

	devlist = kcalloc(1, sizeof(*devlist), GFP_KERNEL);
	if (!devlist)
		return -ENOMEM;

	devlist->nslots = FAMFS_MAX_DAXDEVS;
	devlist->devlist = kcalloc(FAMFS_MAX_DAXDEVS, sizeof(struct famfs_daxdev),
				   GFP_KERNEL);
	if (!devlist->devlist) {
		kfree(devlist);
		return -ENOMEM;
	}

	/* If another thread allocated it first, drop ours */
	if (cmpxchg(&fsi->dax_devlist, NULL, devlist) != NULL) {
		kfree(devlist->devlist);
		kfree(devlist);
	}

	return 0;
}

/*
 * famfs_install_daxdev() - exclusively acquire a resolved daxdev and publish
 * it in the table at @index. Slot 0 is the mount primary; slots 1..n come from
 * the daxdev-open ioctl.
 *
 * Serializes with concurrent installers under devlist_sem and rechecks
 * ->valid, so re-registering an already-installed slot is idempotent. A daxdev
 * is entered in the table only once it has been exclusively acquired via
 * fs_dax_get() (with the super_block as the holder); on failure the
 * dax_dev_find() reference is released and the slot is left invalid. @name may
 * be NULL (the ioctl path passes no pathname).
 */
int
famfs_install_daxdev(
	struct famfs_fs_info *fsi,
	struct super_block *sb,
	u64 index,
	dev_t devno,
	const char *name)
{
	struct famfs_daxdev *daxdev;
	int rc = 0;

	if (index >= fsi->dax_devlist->nslots) {
		pr_debug("%s: index(%llu) >= nslots(%d)\n",
		       __func__, index, fsi->dax_devlist->nslots);
		return -EINVAL;
	}

	scoped_guard(rwsem_write, &fsi->devlist_sem) {
		daxdev = &fsi->dax_devlist->devlist[index];

		/* Installed already by a concurrent (or repeated) open */
		if (daxdev->valid)
			return 0;

		/*
		 * A prior attempt already determined this daxdev cannot be
		 * exclusively acquired (see the fs_dax_get() failure handling
		 * below). Don't thrash on fs_dax_get(); fail fast.
		 */
		if (daxdev->dax_err)
			return -EIO;

		daxdev->devp = dax_dev_find(devno);
		if (!daxdev->devp) {
			pr_debug("%s: device %u:%u not found or not dax\n",
				__func__, MAJOR(devno), MINOR(devno));
			return -ENODEV;
		}

		rc = fs_dax_get(daxdev->devp, sb, &famfs_dax_holder_ops);
		if (rc) {
			/*
			 * Distinguish a lost race from a real failure. -EBUSY
			 * with the daxdev already held by *this* super_block
			 * means a concurrent acquire won and will publish the
			 * slot valid: not an error, and must not be cached as
			 * dax_err. Any other failure is permanent for this
			 * mount, so record dax_err to stop re-acquiring it.
			 */
			if (!(rc == -EBUSY && dax_holder(daxdev->devp) == sb)) {
				pr_debug("%s: fs_dax_get(%u:%u) failed rc=%d\n",
				       __func__, MAJOR(devno), MINOR(devno), rc);
				daxdev->dax_err = true;
			}
			put_dax(daxdev->devp);
			daxdev->devp = NULL;
			return rc;
		}

		daxdev->devno = devno;
		if (name) {
			daxdev->name = kstrdup(name, GFP_KERNEL);
			if (!daxdev->name) {
				fs_put_dax(daxdev->devp, sb);
				put_dax(daxdev->devp);
				daxdev->devp = NULL;
				return -ENOMEM;
			}
		}

		wmb(); /* All other fields must be visible before valid */
		daxdev->valid = 1;
	}

	return 0;
}

/*
 * Release every daxdev in the table and free it. Detach the table under
 * devlist_sem so a notify_failure racing teardown either runs first against
 * the live table or observes dax_devlist == NULL and bails.
 */
static
void famfs_devlist_free(
	struct famfs_fs_info *fsi,
	struct super_block *sb)
{
	struct famfs_dax_devlist *devlist __free(kfree) = NULL;
	int i;

	scoped_guard(rwsem_write, &fsi->devlist_sem) {
		devlist = fsi->dax_devlist;
		fsi->dax_devlist = NULL;
	}

	if (!devlist || !devlist->devlist)
		return;

	for (i = 0; i < devlist->nslots; i++) {
		struct famfs_daxdev *dd = &devlist->devlist[i];

		if (!dd->valid)
			continue;

		if (dd->devp) {
			if (!dd->dax_err)
				fs_put_dax(dd->devp, sb);
			put_dax(dd->devp);
		}
		kfree(dd->name);
	}
	kfree(devlist->devlist);
}

/*
 * Record a memory error on the daxdev matching @dax_devp. Searches the table
 * under the write lock (which serializes against famfs_devlist_free()).
 */
static void
famfs_set_daxdev_err(
	struct famfs_fs_info *fsi,
	struct dax_device *dax_devp)
{
	int i;

	scoped_guard(rwsem_write, &fsi->devlist_sem) {
		if (!fsi->dax_devlist)
			return;
		for (i = 0; i < fsi->dax_devlist->nslots; i++) {
			struct famfs_daxdev *dd = &fsi->dax_devlist->devlist[i];

			if (!dd->valid || dd->devp != dax_devp)
				continue;

			dd->error = true;
			/*
			 * Near-term policy: any daxdev memory error shuts down
			 * the whole mount. Finer per-daxdev handling (via
			 * famfs_dax_err() in the resolver) already exists and
			 * can supersede this later.
			 */
			fsi->deverror = true;
			pr_err("%s: memory error on daxdev %s (%d)\n",
			       __func__, dd->name, i);
			return;
		}
	}
	pr_debug("%s: memory error on unrecognized daxdev\n", __func__);
}

/*****************************************************************************
 * fs_context_operations
 */

static void
famfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_magic		= FAMFS_SUPER_MAGIC;
	sb->s_op		= NULL /* famfs_super_ops */;
	sb->s_time_gran		= 1;
}

int
famfs_lookup_daxdev(const char *pathname, dev_t *devno)
{
	struct inode *inode;
	struct path path;
	int err;

	if (!pathname || !*pathname)
		return -EINVAL;

	err = kern_path(pathname, LOOKUP_FOLLOW, &path);
	if (err)
		return err;

	inode = d_backing_inode(path.dentry);
	if (!S_ISCHR(inode->i_mode)) {
		err = -EINVAL;
		goto out_path_put;
	}

	if (!may_open_dev(&path)) {
		err = -EACCES;
		goto out_path_put;
	}

	/* i_rdev is the char dev_t; fs_dax_get() confirms it is dax later */
	*devno = inode->i_rdev;

out_path_put:
	path_put(&path);
	return err;
}

static int
famfs_get_tree(struct fs_context *fc)
{
	struct famfs_fs_info *fsi = fc->s_fs_info;
	struct super_block *sb;
	struct inode *inode;
	dev_t daxdevno;
	int err;

	err = famfs_lookup_daxdev(fc->source, &daxdevno);
	if (err)
		return err;

	/* This will set sb->s_dev=daxdevno */
	sb = sget_dev(fc, daxdevno);
	if (IS_ERR(sb)) {
		pr_debug("%s: sget_dev error\n", __func__);
		return PTR_ERR(sb);
	}

	if (sb->s_root) {
		pr_debug("%s: found a matching superblock for %s\n",
			__func__, fc->source);

		/* We don't expect to find a match by dev_t; if we do, it must
		 * already be mounted, so we bail
		 */
		err = -EBUSY;
		goto deactivate_out;
	} else {
		pr_debug("%s: initializing new superblock for %s\n",
			__func__, fc->source);
		famfs_fill_super(sb, fc);
	}

	/* Install the primary daxdev (from the mount device) at slot 0 */
	err = famfs_devlist_alloc(fsi);
	if (err)
		goto deactivate_out;

	err = famfs_install_daxdev(fsi, sb, 0, daxdevno, fc->source);
	if (err) {
		pr_err("%s: failed to install primary daxdev %s\n",
		       __func__, fc->source);
		goto deactivate_out;
	}

	inode = famfs_get_inode(sb, NULL, S_IFDIR | fsi->mount_opts.mode, 0);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root) {
		pr_debug("%s: d_make_root() failed\n", __func__);
		err = -ENOMEM;
		goto deactivate_out;
	}

	sb->s_flags |= SB_ACTIVE;

	WARN_ON(fc->root);
	fc->root = dget(sb->s_root);
	return 0;

deactivate_out:
	pr_debug("%s: deactivating sb=%llx\n", __func__, (u64)sb);
	deactivate_locked_super(sb);
	return err;
}

/*****************************************************************************/

enum famfs_param {
	Opt_mode,
	Opt_dax,
};

const struct fs_parameter_spec famfs_fs_parameters[] = {
	fsparam_u32oct("mode",	  Opt_mode),
	fsparam_string("dax",     Opt_dax),
	{}
};

static int
famfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct famfs_fs_info *fsi = fc->s_fs_info;
	struct fs_parse_result result;
	int opt;

	opt = fs_parse(fc, famfs_fs_parameters, param, &result);
	if (opt == -ENOPARAM) {
		opt = vfs_parse_fs_param_source(fc, param);
		if (opt != -ENOPARAM)
			return opt;

		return -ENOPARAM;
	}
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_mode:
		fsi->mount_opts.mode = result.uint_32 & S_IALLUGO;
		break;
	case Opt_dax:
		if (strcmp(param->string, "always")) {
			pr_debug("%s: invalid dax mode %s\n",
				  __func__, param->string);
			return -EINVAL;
		}
		break;
	}

	return 0;
}

static void
famfs_free_fc(struct fs_context *fc)
{
	kfree(fc->s_fs_info);
}

static const struct fs_context_operations famfs_context_ops = {
	.free		= famfs_free_fc,
	.parse_param	= famfs_parse_param,
	.get_tree	= famfs_get_tree,
};

static int
famfs_init_fs_context(struct fs_context *fc)
{
	struct famfs_fs_info *fsi;

	fsi = kzalloc_obj(*fsi, GFP_KERNEL);
	if (!fsi)
		return -ENOMEM;

	init_rwsem(&fsi->devlist_sem);
	fsi->mount_opts.mode = FAMFS_DEFAULT_MODE;
	fc->s_fs_info        = fsi;
	fc->ops              = &famfs_context_ops;
	return 0;
}

static void
famfs_kill_sb(struct super_block *sb)
{
	struct famfs_fs_info *fsi = sb->s_fs_info;

	famfs_devlist_free(fsi, sb);

	kill_char_super(sb);

	kfree(fsi);
	sb->s_fs_info = NULL;
}

#define MODULE_NAME "famfs"
static struct file_system_type famfs_fs_type = {
	.owner		  = THIS_MODULE,
	.name		  = MODULE_NAME,
	.init_fs_context  = famfs_init_fs_context,
	.parameters	  = famfs_fs_parameters,
	.kill_sb	  = famfs_kill_sb,
	.fs_flags	  = FS_REQUIRES_DEV,
};

/******************************************************************************
 * Module stuff
 */
#define FAMFS_MODULE_INCOMPLETE 1

static int __init
init_famfs_fs(void)
{
	int rc;

	if (FAMFS_MODULE_INCOMPLETE)
		return -ENODEV;

	rc = register_filesystem(&famfs_fs_type);

	return rc;
}

static void __exit
famfs_exit(void)
{
	unregister_filesystem(&famfs_fs_type);
	pr_info("%s: unregistered\n", __func__);
}

fs_initcall(init_famfs_fs);
module_exit(famfs_exit);

MODULE_AUTHOR("John Groves");
MODULE_DESCRIPTION("Fabric-Attached Memory File System: see famfs.org");
MODULE_LICENSE("GPL");
