// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ioctl to read verity metadata
 *
 * Copyright 2021 Google LLC
 */

#include "fsverity_private.h"

#include <linux/backing-dev.h>
#include <linux/highmem.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>

static int fsverity_read_merkle_tree(struct inode *inode,
				     const struct fsverity_info *vi,
				     void __user *buf, u64 pos, int length)
{
	const u64 end_pos = min(pos + length, vi->tree_params.tree_size);
	struct backing_dev_info *bdi = inode->i_sb->s_bdi;
	const u64 max_ra_bytes = min((u64)bdi->io_pages << PAGE_SHIFT,
				     ULONG_MAX);
	const struct merkle_tree_params *params = &vi->tree_params;
	unsigned int offs_in_block = pos & (params->block_size - 1);
	int retval = 0;
	int err = 0;

	/*
	 * Iterate through each Merkle tree block in the requested range and
	 * copy the requested portion to userspace. Note that we are returning
	 * a byte stream.
	 */
	while (pos < end_pos) {
		unsigned long ra_bytes;
		unsigned int bytes_to_copy;
		struct fsverity_blockbuf block = { };

		ra_bytes = min_t(unsigned long, end_pos - pos, max_ra_bytes);
		bytes_to_copy = min_t(u64, end_pos - pos,
				      params->block_size - offs_in_block);

		err = fsverity_read_merkle_tree_block(inode, &vi->tree_params,
						      FSVERITY_STREAMING_READ,
						      pos - offs_in_block,
						      ra_bytes, &block);
		if (err)
			break;

		if (copy_to_user(buf, block.kaddr + offs_in_block, bytes_to_copy)) {
			fsverity_drop_merkle_tree_block(inode, &block);
			err = -EFAULT;
			break;
		}
		fsverity_drop_merkle_tree_block(inode, &block);

		retval += bytes_to_copy;
		buf += bytes_to_copy;
		pos += bytes_to_copy;

		if (fatal_signal_pending(current))  {
			err = -EINTR;
			break;
		}
		cond_resched();
		offs_in_block = 0;
	}
	return retval ? retval : err;
}

/* Copy the requested portion of the buffer to userspace. */
static int fsverity_read_buffer(void __user *dst, u64 offset, int length,
				const void *src, size_t src_length)
{
	if (offset >= src_length)
		return 0;
	src += offset;
	src_length -= offset;

	length = min_t(size_t, length, src_length);

	if (copy_to_user(dst, src, length))
		return -EFAULT;

	return length;
}

static int fsverity_read_descriptor(struct inode *inode,
				    void __user *buf, u64 offset, int length)
{
	struct fsverity_descriptor *desc;
	size_t desc_size;
	int res;

	res = fsverity_get_descriptor(inode, &desc);
	if (res)
		return res;

	/* don't include the builtin signature */
	desc_size = offsetof(struct fsverity_descriptor, signature);
	desc->sig_size = 0;

	res = fsverity_read_buffer(buf, offset, length, desc, desc_size);

	kfree(desc);
	return res;
}

static int fsverity_read_signature(struct inode *inode,
				   void __user *buf, u64 offset, int length)
{
	struct fsverity_descriptor *desc;
	int res;

	res = fsverity_get_descriptor(inode, &desc);
	if (res)
		return res;

	if (desc->sig_size == 0) {
		res = -ENODATA;
		goto out;
	}

	/*
	 * Include only the builtin signature.  fsverity_get_descriptor()
	 * already verified that sig_size is in-bounds.
	 */
	res = fsverity_read_buffer(buf, offset, length, desc->signature,
				   le32_to_cpu(desc->sig_size));
out:
	kfree(desc);
	return res;
}

/**
 * fsverity_ioctl_read_metadata() - read verity metadata from a file
 * @filp: file to read the metadata from
 * @uarg: user pointer to fsverity_read_metadata_arg
 *
 * Return: length read on success, 0 on EOF, -errno on failure
 */
int fsverity_ioctl_read_metadata(struct file *filp, const void __user *uarg)
{
	struct inode *inode = file_inode(filp);
	const struct fsverity_info *vi;
	struct fsverity_read_metadata_arg arg;
	int length;
	void __user *buf;

	vi = fsverity_get_info(inode);
	if (!vi)
		return -ENODATA; /* not a verity file */
	/*
	 * Note that we don't have to explicitly check that the file is open for
	 * reading, since verity files can only be opened for reading.
	 */

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	if (arg.__reserved)
		return -EINVAL;

	/* offset + length must not overflow. */
	if (arg.offset + arg.length < arg.offset)
		return -EINVAL;

	/* Ensure that the return value will fit in INT_MAX. */
	length = min_t(u64, arg.length, INT_MAX);

	buf = u64_to_user_ptr(arg.buf_ptr);

	switch (arg.metadata_type) {
	case FS_VERITY_METADATA_TYPE_MERKLE_TREE:
		return fsverity_read_merkle_tree(inode, vi, buf, arg.offset,
						 length);
	case FS_VERITY_METADATA_TYPE_DESCRIPTOR:
		return fsverity_read_descriptor(inode, buf, arg.offset, length);
	case FS_VERITY_METADATA_TYPE_SIGNATURE:
		return fsverity_read_signature(inode, buf, arg.offset, length);
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_GPL(fsverity_ioctl_read_metadata);
