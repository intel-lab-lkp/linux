// SPDX-License-Identifier: GPL-2.0
/*
 * famfs - dax file system for shared fabric-attached memory
 *
 * Copyright 2023-2024 Micron Technology, Inc.
 *
 * This file system, originally based on ramfs the dax support from xfs,
 * is intended to allow multiple host systems to mount a common file system
 * view of dax files that map to shared memory.
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/dax.h>
#include <linux/iomap.h>

#include "famfs_internal.h"

/*********************************************************************
 * file_operations
 */

/* Reject I/O to files that aren't in a valid state */
static ssize_t
famfs_file_invalid(struct inode *inode)
{
	if (!IS_DAX(inode)) {
		pr_debug("%s: inode %llx IS_DAX is false\n",
			 __func__, (u64)inode);
		return -ENXIO;
	}
	return 0;
}

static ssize_t
famfs_rw_prep(struct kiocb *iocb, struct iov_iter *ubuf)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	struct super_block *sb = inode->i_sb;
	struct famfs_fs_info *fsi = sb->s_fs_info;
	size_t i_size = i_size_read(inode);
	size_t count = iov_iter_count(ubuf);
	size_t max_count;
	ssize_t rc;

	if (fsi->deverror)
		return -ENODEV;

	rc = famfs_file_invalid(inode);
	if (rc)
		return rc;

	/* Avoid unsigned underflow if position is past EOF */
	if (iocb->ki_pos >= i_size)
		max_count = 0;
	else
		max_count = i_size - iocb->ki_pos;

	if (count > max_count)
		iov_iter_truncate(ubuf, max_count);

	if (!iov_iter_count(ubuf))
		return 0;

	return rc;
}

static ssize_t
famfs_dax_read_iter(struct kiocb *iocb, struct iov_iter	*to)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	ssize_t rc;

	/* dax_iomap_rw() requires i_rwsem held (shared for read) */
	inode_lock_shared(inode);
	rc = famfs_rw_prep(iocb, to);
	if (rc || !iov_iter_count(to)) {
		inode_unlock_shared(inode);
		return rc;
	}

	rc = dax_iomap_rw(iocb, to, NULL /*&famfs_iomap_ops */);
	inode_unlock_shared(inode);

	if (rc > 0)
		file_accessed(iocb->ki_filp);
	return rc;
}

/**
 * famfs_dax_write_iter()
 *
 * We need our own write-iter in order to prevent append
 *
 * @iocb:
 * @from: iterator describing the user memory source for the write
 */
static ssize_t
famfs_dax_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	struct famfs_fs_info *fsi = inode->i_sb->s_fs_info;
	ssize_t rc;

	if (!famfs_opt_enabled(fsi, FAMFS_OPT_WRITE))
		return -EPERM;

	/* dax_iomap_rw() requires i_rwsem held (exclusive for write) */
	inode_lock(inode);
	rc = famfs_rw_prep(iocb, from);
	if (rc || !iov_iter_count(from)) {
		inode_unlock(inode);
		return rc;
	}

	kiocb_modified(iocb); /* mtime/ctime + strip set[e]uid */

	rc = dax_iomap_rw(iocb, from, NULL /*&famfs_iomap_ops*/);
	inode_unlock(inode);
	return rc;
}

const struct file_operations famfs_file_operations = {
	.owner             = THIS_MODULE,

	/* Custom famfs operations */
	.write_iter	   = famfs_dax_write_iter,
	.read_iter	   = famfs_dax_read_iter,
	.unlocked_ioctl    = NULL /*famfs_file_ioctl*/,
	.mmap		   = NULL /* famfs_file_mmap */,

	/* Force PMD alignment for mmap */
	.get_unmapped_area = thp_get_unmapped_area,

	/* Generic Operations */
	.fsync		   = noop_fsync,
	.splice_read	   = copy_splice_read,
	.splice_write	   = iter_file_splice_write,
	.llseek		   = generic_file_llseek,
};

