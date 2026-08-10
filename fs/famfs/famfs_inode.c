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
#include <linux/statfs.h>

#include "famfs_internal.h"

#define FAMFS_DEFAULT_MODE	0755

static const struct inode_operations famfs_file_inode_operations;
static const struct inode_operations famfs_dir_inode_operations;

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
		inode->i_op = &famfs_file_inode_operations;
		inode->i_fop = &famfs_file_operations;
		break;
	case S_IFDIR:
		inode->i_op = &famfs_dir_inode_operations;
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

/***************************************************************************
 * famfs inode_operations
 */

static int
famfs_setattr(
	struct mnt_idmap *idmap,
	struct dentry *dentry,
	struct iattr *iattr)
{
	struct inode *inode = d_inode(dentry);
	struct famfs_fs_info *fsi = inode->i_sb->s_fs_info;

	/* Resizing a famfs file (its size is pinned to the fmap) */
	if ((iattr->ia_valid & ATTR_SIZE) &&
	    !famfs_opt_enabled(fsi, FAMFS_OPT_TRUNCATE) &&
	    iattr->ia_size != i_size_read(inode))
		return -EPERM;
	if ((iattr->ia_valid & ATTR_MODE) &&
	    !famfs_opt_enabled(fsi, FAMFS_OPT_CHMOD))
		return -EPERM;
	if ((iattr->ia_valid & (ATTR_UID | ATTR_GID)) &&
	    !famfs_opt_enabled(fsi, FAMFS_OPT_CHOWN))
		return -EPERM;
	if ((iattr->ia_valid & (ATTR_ATIME | ATTR_MTIME)) &&
	    !famfs_opt_enabled(fsi, FAMFS_OPT_UTIMES))
		return -EPERM;

	return simple_setattr(idmap, dentry, iattr);
}

static const struct inode_operations famfs_file_inode_operations = {
	/* All generic */
	.setattr	   = famfs_setattr,
	.getattr	   = simple_getattr,
};

/*
 * Internal inode creation helper, shared by ->create, ->mkdir, ->mknod and
 * ->symlink. Each of those callers is responsible for its own FAMFS_OPT_*
 * permission check before getting here.
 */
static int
famfs_mknod(
	struct mnt_idmap *idmap,
	struct inode *dir,
	struct dentry *dentry,
	umode_t mode,
	dev_t dev)
{
	struct famfs_fs_info *fsi = dir->i_sb->s_fs_info;
	struct inode *inode;

	if (fsi->deverror)
		return -ENODEV;

	inode = famfs_get_inode(dir->i_sb, dir, mode, dev);
	if (!inode)
		return -ENOSPC;

	d_make_persistent(dentry, inode);
	/* Adding an entry updates the parent directory's mtime/ctime */
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));

	return 0;
}

static struct dentry *
famfs_mkdir(
	struct mnt_idmap *idmap,
	struct inode *dir,
	struct dentry *dentry,
	umode_t mode)
{
	struct famfs_fs_info *fsi = dir->i_sb->s_fs_info;
	int rc;

	if (fsi->deverror)
		return ERR_PTR(-ENODEV);
	if (!famfs_opt_enabled(fsi, FAMFS_OPT_MKDIR))
		return ERR_PTR(-EPERM);

	rc = famfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFDIR, 0);
	if (rc)
		return ERR_PTR(rc);

	inc_nlink(dir);

	return ERR_PTR(0);
}

static int
famfs_create(
	struct mnt_idmap *idmap,
	struct inode *dir,
	struct dentry *dentry,
	umode_t mode,
	bool excl)
{
	struct famfs_fs_info *fsi = dir->i_sb->s_fs_info;

	if (fsi->deverror)
		return -ENODEV;
	if (!famfs_opt_enabled(fsi, FAMFS_OPT_CREATE))
		return -EPERM;

	return famfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFREG, 0);
}

static int
famfs_mknod_op(
	struct mnt_idmap *idmap,
	struct inode *dir,
	struct dentry *dentry,
	umode_t mode,
	dev_t dev)
{
	struct famfs_fs_info *fsi = dir->i_sb->s_fs_info;

	if (!famfs_opt_enabled(fsi, FAMFS_OPT_MKNOD))
		return -EPERM;

	return famfs_mknod(idmap, dir, dentry, mode, dev);
}

static int
famfs_symlink(
	struct mnt_idmap *idmap,
	struct inode *dir,
	struct dentry *dentry,
	const char *symname)
{
	struct famfs_fs_info *fsi = dir->i_sb->s_fs_info;
	struct inode *inode;
	int len, rc;

	if (fsi->deverror)
		return -ENODEV;
	if (!famfs_opt_enabled(fsi, FAMFS_OPT_SYMLINK))
		return -EPERM;

	inode = famfs_get_inode(dir->i_sb, dir, S_IFLNK | 0777, 0);
	if (!inode)
		return -ENOSPC;

	len = strlen(symname) + 1;
	rc = page_symlink(inode, symname, len);
	if (rc) {
		iput(inode);
		return rc;
	}

	d_make_persistent(dentry, inode);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));

	return 0;
}

static int
famfs_link(
	struct dentry *old_dentry,
	struct inode *dir,
	struct dentry *dentry)
{
	struct famfs_fs_info *fsi = dir->i_sb->s_fs_info;

	if (!famfs_opt_enabled(fsi, FAMFS_OPT_LINK))
		return -EPERM;

	return simple_link(old_dentry, dir, dentry);
}

static int
famfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct famfs_fs_info *fsi = dir->i_sb->s_fs_info;

	/* A file with an fmap may only be unlinked when explicitly enabled */
	if (inode->i_private && !famfs_opt_enabled(fsi, FAMFS_OPT_UNLINK))
		return -EPERM;

	return simple_unlink(dir, dentry);
}

static int famfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct famfs_fs_info *fsi = dir->i_sb->s_fs_info;

	if (!famfs_opt_enabled(fsi, FAMFS_OPT_RMDIR))
		return -EPERM;

	return simple_rmdir(dir, dentry);
}

static int
famfs_rename(
	struct mnt_idmap *idmap,
	struct inode *old_dir,
	struct dentry *old_dentry,
	struct inode *new_dir,
	struct dentry *new_dentry,
	unsigned int flags)
{
	struct famfs_fs_info *fsi = old_dir->i_sb->s_fs_info;
	struct inode *target = d_inode(new_dentry);

	if (!famfs_opt_enabled(fsi, FAMFS_OPT_RENAME))
		return -EPERM;

	/*
	 * Renaming over an existing target removes it, so require the same
	 * policy gate that unlink()/rmdir() would. RENAME_EXCHANGE swaps the
	 * two entries rather than removing the target, so it is exempt.
	 */
	if (target && !(flags & RENAME_EXCHANGE)) {
		if (S_ISDIR(target->i_mode)) {
			if (!famfs_opt_enabled(fsi, FAMFS_OPT_RMDIR))
				return -EPERM;
		} else if (target->i_private &&
			   !famfs_opt_enabled(fsi, FAMFS_OPT_UNLINK)) {
			return -EPERM;
		}
	}

	return simple_rename(idmap, old_dir, old_dentry, new_dir, new_dentry,
			     flags);
}

static const struct inode_operations famfs_dir_inode_operations = {
	.create		= famfs_create,
	.lookup		= simple_lookup,
	.link		= famfs_link,
	.unlink		= famfs_unlink,
	.symlink	= famfs_symlink,
	.mkdir		= famfs_mkdir,
	.mknod		= famfs_mknod_op,
	.rmdir		= famfs_rmdir,
	.rename		= famfs_rename,
};

/*****************************************************************************
 * famfs super_operations
 *
 * TODO: implement a famfs_statfs() that shows size, free and available space,
 * etc.
 */

/*
 * famfs_show_options() - Display the mount options in /proc/mounts.
 */
static int
famfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct famfs_fs_info *fsi = root->d_sb->s_fs_info;

	if (fsi->mount_opts.mode != FAMFS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", fsi->mount_opts.mode);

	return 0;
}

static void
famfs_evict_inode(struct inode *inode)
{
	famfs_meta_free((struct famfs_file_meta *)inode->i_private);
	inode->i_private = NULL;
	dax_break_layout_final(inode);
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

/*
 * famfs_statfs() - report device capacity and consumption so 'df' works.
 * @total_capacity is the sum of installed daxdev sizes; @used_capacity is the
 * sum of device bytes mapped by fmaps (superblock + log + data files). Free is
 * the difference - an approximation of the userspace allocator's free space
 * (it ignores allocator gaps / reserved regions), which is fine for df.
 */
static int
famfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct famfs_fs_info *fsi = dentry->d_sb->s_fs_info;
	u64 total, used, free;

	scoped_guard(rwsem_read, &fsi->stats_sem) {
		total = fsi->total_capacity;
		used  = fsi->used_capacity;
	}
	free = total > used ? total - used : 0;

	buf->f_type    = FAMFS_SUPER_MAGIC;
	buf->f_bsize   = PAGE_SIZE;
	buf->f_frsize  = PAGE_SIZE;
	buf->f_blocks  = total >> PAGE_SHIFT;
	buf->f_bfree   = free  >> PAGE_SHIFT;
	buf->f_bavail  = free  >> PAGE_SHIFT;	/* no root reservation */
	buf->f_namelen = NAME_MAX;
	buf->f_fsid    = u64_to_fsid(huge_encode_dev(dentry->d_sb->s_dev));
	return 0;
}

static const struct super_operations famfs_super_ops = {
	.statfs		= famfs_statfs,
	.drop_inode	= inode_just_drop,
	.show_options	= famfs_show_options,
	.evict_inode    = famfs_evict_inode,
};

/*****************************************************************************/

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
	struct dax_device *devp = NULL;
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
		devp = daxdev->devp;
	}

	/* Freshly installed: add its capacity to the statfs accounting */
	if (devp) {
		u64 sz = dax_fsdev_size(devp);

		scoped_guard(rwsem_write, &fsi->stats_sem)
			fsi->total_capacity += sz;
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
	sb->s_op		= &famfs_super_ops;
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
	init_rwsem(&fsi->stats_sem);
	atomic64_set(&fsi->opts, FAMFS_OPT_DEFAULT);
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
static int __init
init_famfs_fs(void)
{
	int rc;

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
