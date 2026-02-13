// SPDX-License-Identifier: GPL-2.0-only

#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/hex.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/seq_file.h>

#include "internal.h"

#define DLMPFS_DEFAULT_MODE	0755

static const struct super_operations dlmpfs_ops;
static const struct inode_operations dlmpfs_dir_inode_operations;

struct dlm_pdata_lvb {
	__le32 inum;
#define DLMPFS_LVB_USED	BIT(0)
	__le32 flags;
};

struct dlmpfs_inode {
	struct inode inode;
	struct dlmlk *lkinum;
	struct dlmlk *lkfpath;
};

static inline struct dlmpfs_inode *DLMPFS_I(struct inode *inode)
{
	return container_of(inode, struct dlmpfs_inode, inode);
}

static struct inode *dlmpfs_alloc_inode(struct super_block *sb)
{
	struct dlmpfs_inode *di;

	di = kzalloc_obj(*di, GFP_NOFS);
	if (!di)
		return NULL;

	inode_init_once(&di->inode);
	return &di->inode;
}

static void dlmpfs_free_inode(struct inode *inode)
{
	struct dlmpfs_inode *ip = DLMPFS_I(inode);

	if (ip->lkfpath) {
		dlmlk_free_nowait(ip->lkfpath);
		ip->lkfpath = NULL;
	}

	if (ip->lkinum) {
		dlmlk_free_nowait(ip->lkinum);
		ip->lkinum = NULL;
	}

	kfree(ip);
}

static void dlmpfs_reverse_hex(char *c, u64 value)
{
	*c = '0';
	while (value) {
		*c-- = hex_asc[value & 0x0f];
		value >>= 4;
	}
}

void dlmpfs_fill_resname_num(const char *prefix, char *resname,
			     unsigned int num)
{
	size_t len;

	memset(resname, 0, DLMPFS_NUM_RESNAME_LEN);
	len = strlen(prefix);
	memcpy(resname, prefix, len);
	resname[len] = ' ';
	dlmpfs_reverse_hex(resname + len + 1, num);
}

static unsigned int dlmpfs_reserve_inum(struct dlmpfs_fs_info *fsi,
					struct dlmpfs_inode *di)
{
	char strname[DLMPFS_NUM_RESNAME_LEN];
	unsigned int num = fsi->last_inum;
	struct dlm_pdata_lvb *lvb;

retry_inum:
	num++;
	if (!num)
		num++;

	if (!di->lkinum) {
		dlmpfs_fill_resname_num("inode", strname, num);
		di->lkinum = dlmlk_alloc(fsi->ls, strname, strlen(strname));
	}

	dlmlk_convert(di->lkinum, DLM_LOCK_EX, DLM_LKF_VALBLK);

	lvb = dlmlk_lvb(di->lkinum);
	if (lvb->flags & cpu_to_le32(DLMPFS_LVB_USED)) {
		dlmlk_free(di->lkinum);
		di->lkinum = NULL;
		goto retry_inum;
	}

	lvb->flags |= cpu_to_le32(DLMPFS_LVB_USED);
	dlmlk_convert(di->lkinum, DLM_LOCK_NL, DLM_LKF_VALBLK);
	fsi->last_inum = num;
	return num;
}

static int dlmpfs_add_inum_usage(struct dlmpfs_fs_info *fsi,
				 struct dlmpfs_inode *di,
				 __le32 num)
{
	char strname[DLMPFS_NUM_RESNAME_LEN];
	struct dlm_pdata_lvb *lvb;

	if (!di->lkinum) {
		dlmpfs_fill_resname_num("inode", strname, le32_to_cpu(num));
		di->lkinum = dlmlk_alloc(fsi->ls, strname, strlen(strname));
	}

	dlmlk_convert(di->lkinum, DLM_LOCK_NL, DLM_LKF_VALBLK);

	lvb = dlmlk_lvb(di->lkinum);
	if (!(lvb->flags & cpu_to_le32(DLMPFS_LVB_USED))) {
		dlmlk_free(di->lkinum);
		di->lkinum = NULL;
		/* failed to add usage */
		return 1;
	}

	return 0;
}

static unsigned int dlmpfs_alloc_inode_num(struct dlmpfs_fs_info *fsi,
					   struct dlmpfs_inode *di,
					   const void *res, size_t reslen)
{
	struct dlm_pdata_lvb *lvb;
	unsigned int num;
	int rv;

	if (!di->lkfpath)
		di->lkfpath = dlmlk_alloc(fsi->ls, res, reslen);

retry:
	dlmlk_convert(di->lkfpath, DLM_LOCK_NL, DLM_LKF_VALBLK);

	lvb = dlmlk_lvb(di->lkfpath);
	/* cluster wide inum being set */
	if (lvb->flags & cpu_to_le32(DLMPFS_LVB_USED)) {
		/* increment inode user */
		rv = dlmpfs_add_inum_usage(fsi, di, lvb->inum);
		if (rv)
			goto retry;

		return le32_to_cpu(lvb->inum);
	}

	dlmlk_convert(di->lkfpath, DLM_LOCK_EX, DLM_LKF_VALBLK);
	if (lvb->flags & cpu_to_le32(DLMPFS_LVB_USED)) {
		/* increment inode user */
		rv = dlmpfs_add_inum_usage(fsi, di, lvb->inum);
		if (rv)
			goto retry;

		dlmlk_convert(di->lkfpath, DLM_LOCK_NL, 0);
		return le32_to_cpu(lvb->inum);
	}

	num = dlmpfs_reserve_inum(fsi, di);
	lvb->flags |= cpu_to_le32(DLMPFS_LVB_USED);
	lvb->inum = cpu_to_le32(num);

	dlmlk_convert(di->lkfpath, DLM_LOCK_NL, DLM_LKF_VALBLK);
	return num;
}

static struct dlmpfs_inode *dlmpfs_new_inode(struct super_block *sb)
{
	return DLMPFS_I(new_inode(sb));
}

static struct inode *dlmpfs_get_inode(struct super_block *sb, const void *res,
				      size_t reslen, const struct inode *dir,
				      umode_t mode, dev_t dev)
{
	struct dlmpfs_inode *dinode = dlmpfs_new_inode(sb);
	struct dlmpfs_fs_info *fsi = sb->s_fs_info;
	struct inode *inode;

	if (dinode) {
		inode = &dinode->inode;
		inode->i_ino = dlmpfs_alloc_inode_num(fsi, dinode, res, reslen);
		inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
		simple_inode_init_ts(inode);
		switch (mode & S_IFMT) {
		case S_IFREG:
			inode->i_op = &dlmpfs_file_inode_operations;
			inode->i_fop = &dlmpfs_file_operations;
			break;
		case S_IFDIR:
			inode->i_op = &dlmpfs_dir_inode_operations;
			inode->i_fop = &simple_dir_operations;

			/* directory inodes start off with i_nlink == 2 (for "." entry) */
			inc_nlink(inode);
			break;
		default:
			WARN_ON(1);
			break;
		}
	}
	return inode;
}

/*
 * File creation. Allocate an inode, and we're done..
 */
/* SMP-safe */
static int
dlmpfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
	     struct dentry *dentry, umode_t mode, dev_t dev)
{
	unsigned char *resname[DLM_RESNAME_MAXLEN] = {};
	struct inode *inode;
	char *path, *pbuf;
	size_t pathlen;
	int error;

	switch (mode & S_IFMT) {
	case S_IFREG:
		break;
	case S_IFDIR:
		break;
	default:
		return -EOPNOTSUPP;
	}

	pbuf = (char *)__get_free_page(GFP_KERNEL);
	if (!pbuf)
		return -ENOSPC;

	path = dentry_path_raw(dentry, pbuf, PAGE_SIZE);
	if (IS_ERR(path)) {
		free_page((unsigned long)pbuf);
		return -ENOSPC;
	}

	pathlen = strlen(path);
	if (pathlen > DLM_RESNAME_MAXLEN) {
		error = -ENAMETOOLONG;
		goto out;
	}

	memcpy(resname, path, pathlen);
	/* use next power of 2 as pathlen */
	pathlen = ALIGN(pathlen, 8);
	error = -ENOSPC;
	inode = dlmpfs_get_inode(dir->i_sb, resname, pathlen, dir, mode, dev);
	if (inode) {
		error = security_inode_init_security(inode, dir,
						     &dentry->d_name, NULL,
						     NULL);
		if (error) {
			iput(inode);
			goto out;
		}

		d_make_persistent(dentry, inode);
		error = 0;
		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	}

out:
	free_page((unsigned long)pbuf);
	return error;
}

static struct dentry *dlmpfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				   struct dentry *dentry, umode_t mode)
{
	int retval = dlmpfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFDIR, 0);

	if (!retval)
		inc_nlink(dir);
	return ERR_PTR(retval);
}

static int dlmpfs_create(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode, bool excl)
{
	return dlmpfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFREG, 0);
}

static const struct inode_operations dlmpfs_dir_inode_operations = {
	.create		= dlmpfs_create,
	.lookup		= simple_lookup,
	.mkdir		= dlmpfs_mkdir,
	.rmdir		= simple_rmdir,
	.unlink         = simple_unlink,
	.mknod          = dlmpfs_mknod,
};

/*
 * Display the mount options in /proc/mounts.
 */
static int dlmpfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct dlmpfs_fs_info *fsi = root->d_sb->s_fs_info;

	if (fsi->mount_opts.mode != DLMPFS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", fsi->mount_opts.mode);
	return 0;
}

static const struct super_operations dlmpfs_ops = {
	.alloc_inode	= dlmpfs_alloc_inode,
	.free_inode	= dlmpfs_free_inode,
	.statfs		= simple_statfs,
	.drop_inode	= inode_just_drop,
	.show_options	= dlmpfs_show_options,
};

enum dlmpfs_param {
	Opt_mode,
	Opt_clname,
	Opt_lsname,
};

static const struct fs_parameter_spec dlmpfs_fs_parameters[] = {
	fsparam_u32oct("mode",	Opt_mode),
	fsparam_string("clname",	Opt_clname),
	fsparam_string("lsname",	Opt_lsname),
	{}
};

static int dlmpfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct dlmpfs_fs_info *fsi = fc->s_fs_info;
	struct fs_parse_result result;
	int opt;

	opt = fs_parse(fc, dlmpfs_fs_parameters, param, &result);
	if (opt == -ENOPARAM) {
		opt = vfs_parse_fs_param_source(fc, param);
		if (opt != -ENOPARAM)
			return opt;
		/*
		 * We might like to report bad mount options here;
		 * but traditionally dlmpfs has ignored all mount options,
		 * and as it is used as a !CONFIG_SHMEM simple substitute
		 * for tmpfs, better continue to ignore other mount options.
		 */
		return 0;
	}
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_mode:
		fsi->mount_opts.mode = result.uint_32 & S_IALLUGO;
		break;
	case Opt_clname:
		strscpy(fsi->clname, param->string, DLM_RESNAME_MAXLEN);
		break;
	case Opt_lsname:
		strscpy(fsi->lsname, param->string, DLM_RESNAME_MAXLEN);
		break;
	}

	return 0;
}

static int dlmpfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct dlmpfs_fs_info *fsi = sb->s_fs_info;
	unsigned char res[8] = {};
	struct inode *inode;
	int rv;

	rv = dlmls_new(fsi->lsname, fsi->clname, 0, 8, NULL, NULL,
		       NULL, &fsi->ls);
	if (rv) {
		fsi->ls = NULL;
		return rv;
	}

	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_magic		= DLMPFS_MAGIC;
	sb->s_op		= &dlmpfs_ops;
	sb->s_d_flags		= DCACHE_DONTCACHE;
	sb->s_time_gran		= 1;

	res[0] = '/';
	inode = dlmpfs_get_inode(sb, res, sizeof(res), NULL, S_IFDIR | fsi->mount_opts.mode, 0);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}

static int dlmpfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, dlmpfs_fill_super);
}

static void dlmpfs_free_fc(struct fs_context *fc)
{
	kfree(fc->s_fs_info);
}

static const struct fs_context_operations dlmpfs_context_ops = {
	.free		= dlmpfs_free_fc,
	.parse_param	= dlmpfs_parse_param,
	.get_tree	= dlmpfs_get_tree,
};

static int dlmpfs_init_fs_context(struct fs_context *fc)
{
	struct dlmpfs_fs_info *fsi;

	fsi = kzalloc_obj(*fsi, GFP_KERNEL);
	if (!fsi)
		return -ENOMEM;

	strscpy(fsi->clname, "cluster");
	strscpy(fsi->lsname, "dlmpfs");
	fsi->mount_opts.mode = DLMPFS_DEFAULT_MODE;
	fc->s_fs_info = fsi;
	fc->ops = &dlmpfs_context_ops;
	return 0;
}

static void dlmpfs_kill_sb(struct super_block *sb)
{
	struct dlmpfs_fs_info *fsi = sb->s_fs_info;
	struct dlmls *ls = fsi->ls;

	kfree(sb->s_fs_info);
	kill_anon_super(sb);

	if (ls)
		dlmls_release(ls, 2);
}

static struct file_system_type dlmpfs_fs_type = {
	.name		= "dlmpfs",
	.init_fs_context = dlmpfs_init_fs_context,
	.parameters	= dlmpfs_fs_parameters,
	.kill_sb	= dlmpfs_kill_sb,
};

static int __init init_dlmpfs_fs(void)
{
	return register_filesystem(&dlmpfs_fs_type);
}
fs_initcall(init_dlmpfs_fs);
