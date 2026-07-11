// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992 Linus Torvalds
 *
 *  minix regular file handling primitives
 */

#include <linux/buffer_head.h>
#include "minix.h"

int minix_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	return mmb_fsync(file,
			&minix_i(file->f_mapping->host)->i_metadata_bhs,
			start, end, datasync);
}

static ssize_t minix_dio_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	ssize_t ret;

	inode_lock_shared(inode);

	const struct iomap_ops *ops = minix_iomap_ops_ver(inode);

	ret = iomap_dio_rw(iocb, to, ops, NULL, 0, NULL, 0);
	inode_unlock_shared(inode);
	return ret;
}

static int minix_dio_write_end_io(struct kiocb *iocb, ssize_t size, int error,
		unsigned int flags)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	loff_t pos = iocb->ki_pos;

	if (error)
		return error;

	pos += size;
	if (size && pos > i_size_read(inode)) {
		i_size_write(inode, pos);
		mark_inode_dirty(inode);
	}
	return 0;
}

static const struct iomap_dio_ops minix_dio_write_ops = {
	.end_io = minix_dio_write_end_io,
};

static ssize_t minix_dio_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	ssize_t ret;
	unsigned int flags = 0;
	unsigned long blocksize = inode->i_sb->s_blocksize;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out_unlock;

	ret = kiocb_modified(iocb);
	if (ret)
		goto out_unlock;

	if (iocb->ki_pos + iov_iter_count(from) > i_size_read(inode) ||
		!IS_ALIGNED(iocb->ki_pos | iov_iter_alignment(from), blocksize))
		flags |= IOMAP_DIO_FORCE_WAIT;

	const struct iomap_ops *ops = minix_iomap_ops_ver(inode);

	ret = iomap_dio_rw(iocb, from, ops,
		&minix_dio_write_ops, flags, NULL, 0);
	if (ret == -ENOTBLK)
		ret = 0; /* fallback to buffered */

	if (ret >= 0 && iov_iter_count(from)) {
		loff_t pos;
		loff_t endbyte;
		ssize_t status;

		iocb->ki_flags &= ~IOCB_DIRECT;
		iocb->ki_flags |= IOCB_DSYNC;
		pos = iocb->ki_pos;
		status = iomap_file_buffered_write(iocb, from, ops,
			NULL, NULL);
		if (unlikely(status < 0)) {
			ret = status;
			goto out_unlock;
		}

		ret += status;
		endbyte = pos + status - 1;
		invalidate_mapping_pages(inode->i_mapping,
			pos >> PAGE_SHIFT,
			endbyte >> PAGE_SHIFT);
		if (ret > 0)
			ret = generic_write_sync(iocb, ret);
	}

out_unlock:
	inode_unlock(inode);
	return ret;
}

static ssize_t minix_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	if (iocb->ki_flags & IOCB_DIRECT)
		return minix_dio_read_iter(iocb, to);

	return generic_file_read_iter(iocb, to);
}

static ssize_t minix_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	ssize_t ret;

	/* minix_dio_write_iter also locks the inode and appears to do the same
	 * general sorts of checks as this, so just return directly from there.
	 */
	if (iocb->ki_flags & IOCB_DIRECT)
		return minix_dio_write_iter(iocb, from);

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto unlock;

	ret = file_modified(iocb->ki_filp);
	if (ret)
		goto unlock;

	const struct iomap_ops *ops = minix_iomap_ops_ver(inode);

	ret = iomap_file_buffered_write(iocb, from, ops,
			NULL, NULL);

	if (ret > 0)
		ret = generic_write_sync(iocb, ret);

unlock:
	inode_unlock(inode);
	return ret;
}

static int minix_file_open(struct inode *inode, struct file *filp)
{
	filp->f_mode |= FMODE_CAN_ODIRECT;
	return generic_file_open(inode, filp);
}

/*
 * We still have some NULLs here, but not as many of the current defaults are
 * still OK for the minix filesystem.
 */

const struct file_operations minix_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= minix_file_read_iter,
	.write_iter	= minix_file_write_iter,
	.mmap_prepare	= generic_file_mmap_prepare,
	.open		= minix_file_open,
	.fsync		= minix_fsync,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
};

int minix_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
	struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int error;

	error = setattr_prepare(&nop_mnt_idmap, dentry, attr);
	if (error)
		return error;

	if ((attr->ia_valid & ATTR_SIZE) &&
	    attr->ia_size != i_size_read(inode)) {
		error = inode_newsize_ok(inode, attr->ia_size);
		if (error)
			return error;

		truncate_setsize(inode, attr->ia_size);
		minix_truncate(inode);
	}

	setattr_copy(&nop_mnt_idmap, inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

const struct inode_operations minix_file_inode_operations = {
	.setattr	= minix_setattr,
	.getattr	= minix_getattr,
};
