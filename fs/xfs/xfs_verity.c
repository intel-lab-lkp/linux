// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Red Hat, Inc.
 */
#include "xfs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_attr.h"
#include "xfs_verity.h"
#include "xfs_bmap_util.h"
#include "xfs_log_format.h"
#include "xfs_trans.h"

static int
xfs_get_verity_descriptor(
	struct inode		*inode,
	void			*buf,
	size_t			buf_size)
{
	struct xfs_inode	*ip = XFS_I(inode);
	int			error = 0;
	struct xfs_da_args	args = {
		.dp		= ip,
		.attr_filter	= XFS_ATTR_VERITY,
		.name		= (const uint8_t *)XFS_VERITY_DESCRIPTOR_NAME,
		.namelen	= XFS_VERITY_DESCRIPTOR_NAME_LEN,
		.value		= buf,
		.valuelen	= buf_size,
	};

	/*
	 * The fact that (returned attribute size) == (provided buf_size) is
	 * checked by xfs_attr_copy_value() (returns -ERANGE)
	 */
	error = xfs_attr_get(&args);
	if (error)
		return error;

	return args.valuelen;
}

static int
xfs_begin_enable_verity(
	struct file	    *filp)
{
	struct inode	    *inode = file_inode(filp);
	struct xfs_inode    *ip = XFS_I(inode);
	int		    error = 0;

	ASSERT(xfs_isilocked(ip, XFS_IOLOCK_EXCL));

	if (IS_DAX(inode))
		return -EINVAL;

	if (xfs_iflags_test_and_set(ip, XFS_IVERITY_CONSTRUCTION))
		return -EBUSY;

	return error;
}

static int
xfs_drop_merkle_tree(
	struct xfs_inode	*ip,
	u64			merkle_tree_size,
	u8			log_blocksize)
{
	struct xfs_fsverity_merkle_key	name;
	int			error = 0, index;
	u64			offset = 0;
	struct xfs_da_args	args = {
		.dp		= ip,
		.whichfork	= XFS_ATTR_FORK,
		.attr_filter	= XFS_ATTR_VERITY,
		.namelen	= sizeof(struct xfs_fsverity_merkle_key),
		/* NULL value make xfs_attr_set remove the attr */
		.value		= NULL,
	};

	for (index = 1; offset < merkle_tree_size; index++) {
		xfs_fsverity_merkle_key_to_disk(&name, offset);
		args.name = (const uint8_t *)&name.merkleoff;
		args.attr_filter = XFS_ATTR_VERITY;
		error = xfs_attr_set(&args);
		offset = index << log_blocksize;
	}

	args.name = (const uint8_t *)XFS_VERITY_DESCRIPTOR_NAME;
	args.namelen = XFS_VERITY_DESCRIPTOR_NAME_LEN;
	args.attr_filter = XFS_ATTR_VERITY;
	error = xfs_attr_set(&args);

	return error;
}

static int
xfs_end_enable_verity(
	struct file		*filp,
	const void		*desc,
	size_t			desc_size,
	u64			merkle_tree_size,
	u8			log_blocksize)
{
	struct inode		*inode = file_inode(filp);
	struct xfs_inode	*ip = XFS_I(inode);
	struct xfs_mount	*mp = ip->i_mount;
	struct xfs_trans	*tp;
	struct xfs_da_args	args = {
		.dp		= ip,
		.whichfork	= XFS_ATTR_FORK,
		.attr_filter	= XFS_ATTR_VERITY,
		.attr_flags	= XATTR_CREATE,
		.name		= (const uint8_t *)XFS_VERITY_DESCRIPTOR_NAME,
		.namelen	= XFS_VERITY_DESCRIPTOR_NAME_LEN,
		.value		= (void *)desc,
		.valuelen	= desc_size,
	};
	int			error = 0;

	ASSERT(xfs_isilocked(ip, XFS_IOLOCK_EXCL));

	/* fs-verity failed, just cleanup */
	if (desc == NULL)
		goto out;

	error = xfs_attr_set(&args);
	if (error)
		goto out;

	/* Set fsverity inode flag */
	error = xfs_trans_alloc_inode(ip, &M_RES(mp)->tr_ichange,
			0, 0, false, &tp);
	if (error)
		goto out;

	/*
	 * Ensure that we've persisted the verity information before we enable
	 * it on the inode and tell the caller we have sealed the inode.
	 */
	ip->i_diflags2 |= XFS_DIFLAG2_VERITY;

	xfs_trans_log_inode(tp, ip, XFS_ILOG_CORE);
	xfs_trans_set_sync(tp);

	error = xfs_trans_commit(tp);
	xfs_iunlock(ip, XFS_ILOCK_EXCL);

	if (!error)
		inode->i_flags |= S_VERITY;

out:
	if (error)
		WARN_ON_ONCE(xfs_drop_merkle_tree(ip, merkle_tree_size,
						  log_blocksize));

	xfs_iflags_clear(ip, XFS_IVERITY_CONSTRUCTION);
	return error;
}

int
xfs_read_merkle_tree_block(
	struct inode		*inode,
	unsigned int		pos,
	struct fsverity_block	*block,
	unsigned long		num_ra_pages)
{
	struct xfs_inode	*ip = XFS_I(inode);
	struct xfs_fsverity_merkle_key name;
	int			error = 0;
	struct xfs_da_args	args = {
		.dp		= ip,
		.attr_filter	= XFS_ATTR_VERITY,
		.namelen	= sizeof(struct xfs_fsverity_merkle_key),
	};
	xfs_fsverity_merkle_key_to_disk(&name, pos);
	args.name = (const uint8_t *)&name.merkleoff;

	error = xfs_attr_get(&args);
	if (error)
		goto out;

	WARN_ON_ONCE(!args.valuelen);

	/* now we also want to get underlying xfs_buf */
	args.op_flags = XFS_DA_OP_BUFFER;
	error = xfs_attr_get(&args);
	if (error)
		goto out;

	block->kaddr = args.value;
	block->len = args.valuelen;
	block->cached = args.bp->b_flags & XBF_VERITY_CHECKED;
	block->context = args.bp;

	return error;

out:
	kmem_free(args.value);
	if (args.bp)
		xfs_buf_rele(args.bp);
	return error;
}

static int
xfs_write_merkle_tree_block(
	struct inode		*inode,
	const void		*buf,
	u64			pos,
	unsigned int		size)
{
	struct xfs_inode	*ip = XFS_I(inode);
	struct xfs_fsverity_merkle_key	name;
	struct xfs_da_args	args = {
		.dp		= ip,
		.whichfork	= XFS_ATTR_FORK,
		.attr_filter	= XFS_ATTR_VERITY,
		.attr_flags	= XATTR_CREATE,
		.namelen	= sizeof(struct xfs_fsverity_merkle_key),
		.value		= (void *)buf,
		.valuelen	= size,
	};

	xfs_fsverity_merkle_key_to_disk(&name, pos);
	args.name = (const uint8_t *)&name.merkleoff;

	return xfs_attr_set(&args);
}

static void
xfs_drop_block(
	struct fsverity_block	*block)
{
	struct xfs_buf		*buf;

	ASSERT(block != NULL);

	buf = (struct xfs_buf *)block->context;

	if (block->cached)
		buf->b_flags |= XBF_VERITY_CHECKED;
	xfs_buf_rele(buf);

	kunmap_local(block->kaddr);
}

const struct fsverity_operations xfs_verity_ops = {
	.begin_enable_verity		= &xfs_begin_enable_verity,
	.end_enable_verity		= &xfs_end_enable_verity,
	.get_verity_descriptor		= &xfs_get_verity_descriptor,
	.read_merkle_tree_block		= &xfs_read_merkle_tree_block,
	.write_merkle_tree_block	= &xfs_write_merkle_tree_block,
	.drop_block			= &xfs_drop_block,
};
