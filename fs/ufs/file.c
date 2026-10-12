// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ufs/file.c
 *
 * Copyright (C) 1998
 * Daniel Pirkl <daniel.pirkl@email.cz>
 * Charles University, Faculty of Mathematics and Physics
 *
 *  from
 *
 *  linux/fs/ext2/file.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext2 fs regular file handling primitives
 */

#include <linux/fs.h>
#include <linux/filelock.h>

#include "ufs_fs.h"
#include "ufs.h"

static ssize_t ufs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	if (ufs_emergency_ro(file_inode(iocb->ki_filp)->i_sb))
		return -EROFS;
	return generic_file_write_iter(iocb, from);
}

/* Inodes aren't written back after an error, don't claim they're on disk. */
int ufs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	if (ufs_emergency_ro(file_inode(file)->i_sb))
		return -EROFS;
	return simple_fsync(file, start, end, datasync);
}

static int ufs_file_open(struct inode *inode, struct file *filp)
{
	if ((filp->f_mode & FMODE_WRITE) && ufs_emergency_ro(inode->i_sb))
		return -EROFS;
	return generic_file_open(inode, filp);
}

/*
 * We have mostly NULL's here: the current defaults are ok for
 * the ufs filesystem.
 */
 
const struct file_operations ufs_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= ufs_file_write_iter,
	.mmap_prepare	= generic_file_mmap_prepare,
	.open           = ufs_file_open,
	.fsync		= ufs_fsync,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.setlease	= generic_setlease,
};
