// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE NFS export support.
 *
 * Copyright (C) 2001-2008  Miklos Szeredi <miklos@szeredi.hu>
 * Copyright (C) 2025 Jump Trading LLC, author: Luis Henriques <luis@igalia.com>
 */

#include "fuse_i.h"
#include <linux/exportfs.h>

struct fuse_inode_handle {
	u64 nodeid;
	u32 generation; /* XXX change to u64, and use fid->i64.ino in encode/decode? */
	struct fuse_file_handle fh;
};

static struct dentry *fuse_get_dentry(struct super_block *sb,
				      struct fuse_inode_handle *handle)
{
	struct fuse_conn *fc = get_fuse_conn_super(sb);
	struct inode *inode;
	struct dentry *entry;
	int err = -ESTALE;

	if (handle->nodeid == 0)
		goto out_err;

	inode = ilookup5(sb, handle->nodeid, fuse_inode_eq, &handle->nodeid);
	if (!inode) {
		struct fuse_entry_out *outarg;
		const struct qstr name = QSTR_INIT(".", 1);

		if (!fc->export_support)
			goto out_err;

		outarg = fuse_entry_out_alloc(fc);
		if (!outarg) {
			err = -ENOMEM;
			goto out_err;
		}

		err = fuse_lookup_name(sb, handle->nodeid, NULL, &name, outarg,
				       &inode);
		kfree(outarg);
		if (err && err != -ENOENT)
			goto out_err;
		if (err || !inode) {
			err = -ESTALE;
			goto out_err;
		}
		err = -EIO;
		if (get_node_id(inode) != handle->nodeid)
			goto out_iput;
	}
	err = -ESTALE;
	if (inode->i_generation != handle->generation)
		goto out_iput;

	entry = d_obtain_alias(inode);
	if (!IS_ERR(entry) && get_node_id(inode) != FUSE_ROOT_ID)
		fuse_invalidate_entry_cache(entry);

	return entry;

 out_iput:
	iput(inode);
 out_err:
	return ERR_PTR(err);
}

static int fuse_encode_gen_fh(struct inode *inode, u32 *fh, int *max_len,
			      struct inode *parent)
{
	int len = parent ? 6 : 3;
	u64 nodeid;
	u32 generation;

	if (*max_len < len) {
		*max_len = len;
		return  FILEID_INVALID;
	}

	nodeid = get_fuse_inode(inode)->nodeid;
	generation = inode->i_generation;

	fh[0] = (u32)(nodeid >> 32);
	fh[1] = (u32)(nodeid & 0xffffffff);
	fh[2] = generation;

	if (parent) {
		nodeid = get_fuse_inode(parent)->nodeid;
		generation = parent->i_generation;

		fh[3] = (u32)(nodeid >> 32);
		fh[4] = (u32)(nodeid & 0xffffffff);
		fh[5] = generation;
	}

	*max_len = len;

	return parent ? FILEID_INO64_GEN_PARENT : FILEID_INO64_GEN;
}

static int fuse_encode_fuse_fh(struct inode *inode, u32 *fh, int *max_len,
			       struct inode *parent)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct fuse_inode *fip = NULL;
	struct fuse_inode_handle *handle = (void *)fh;
	int type = FILEID_FUSE_WITHOUT_PARENT;
	int len, lenp = 0;
	int buflen = *max_len << 2; /* max_len: number of words */

	len = sizeof(struct fuse_inode_handle) + fi->fh->size;
	if (parent) {
		fip = get_fuse_inode(parent);
		if (fip->fh && fip->fh->size) {
			lenp = sizeof(struct fuse_inode_handle) +
				fip->fh->size;
			type = FILEID_FUSE_WITH_PARENT;
		}
	}

	if (buflen < (len + lenp)) {
		*max_len = (len + lenp) >> 2;
		return  FILEID_INVALID;
	}

	handle[0].nodeid = fi->nodeid;
	handle[0].generation = inode->i_generation;
	memcpy(&handle[0].fh, fi->fh, len);
	if (lenp) {
		handle[1].nodeid = fip->nodeid;
		handle[1].generation = parent->i_generation;
		memcpy(&handle[1].fh, fip->fh, lenp);
	}

	*max_len = (len + lenp) >> 2;

	return type;
}

static int fuse_encode_fh(struct inode *inode, u32 *fh, int *max_len,
			   struct inode *parent)
{
	struct fuse_conn *fc = get_fuse_conn(inode);
	struct fuse_inode *fi = get_fuse_inode(inode);

	if (fc->lookup_handle && fi->fh && fi->fh->size)
		return fuse_encode_fuse_fh(inode, fh, max_len, parent);

	return fuse_encode_gen_fh(inode, fh, max_len, parent);
}

static struct dentry *fuse_fh_gen_to_dentry(struct super_block *sb,
					    struct fid *fid, int fh_len)
{
	struct fuse_inode_handle handle;

	if (fh_len < 3)
		return NULL;

	handle.nodeid = (u64) fid->raw[0] << 32;
	handle.nodeid |= (u64) fid->raw[1];
	handle.generation = fid->raw[2];

	return fuse_get_dentry(sb, &handle);
}

static struct dentry *fuse_fh_fuse_to_dentry(struct super_block *sb,
					     struct fid *fid, int fh_len)
{
	struct fuse_inode_handle *handle;
	struct dentry *dentry;
	int len = sizeof(struct fuse_file_handle);

	handle = (void *)fid;
	len += handle->fh.size;

	if ((fh_len << 2) < len)
		return NULL;

	handle = kzalloc(len, GFP_KERNEL);
	if (!handle)
		return NULL;

	memcpy(handle, fid, len);

	dentry = fuse_get_dentry(sb, handle);
	kfree(handle);

	return dentry;
}

static struct dentry *fuse_fh_to_dentry(struct super_block *sb,
		struct fid *fid, int fh_len, int fh_type)
{
	switch (fh_type) {
	case FILEID_INO64_GEN:
	case FILEID_INO64_GEN_PARENT:
		return fuse_fh_gen_to_dentry(sb, fid, fh_len);
	case FILEID_FUSE_WITHOUT_PARENT:
	case FILEID_FUSE_WITH_PARENT:
		return fuse_fh_fuse_to_dentry(sb, fid, fh_len);
	}

	return NULL;

}

static struct dentry *fuse_fh_gen_to_parent(struct super_block *sb,
					    struct fid *fid, int fh_len)
{
	struct fuse_inode_handle parent;

	if (fh_len < 6)
		return NULL;

	parent.nodeid = (u64) fid->raw[3] << 32;
	parent.nodeid |= (u64) fid->raw[4];
	parent.generation = fid->raw[5];

	return fuse_get_dentry(sb, &parent);
}

static struct dentry *fuse_fh_fuse_to_parent(struct super_block *sb,
					     struct fid *fid, int fh_len)
{
	struct fuse_inode_handle *handle;
	struct dentry *dentry;
	int total_len;
	int len;

	handle = (void *)fid;
	total_len = len = sizeof(struct fuse_inode_handle) + handle->fh.size;

	if ((fh_len << 2) < total_len)
		return NULL;

	handle = (void *)(fid + len);
	len = sizeof(struct fuse_file_handle) + handle->fh.size;
	total_len += len;

	if ((fh_len << 2) < total_len)
		return NULL;
	
	handle = kzalloc(len, GFP_KERNEL);
	if (!handle)
		return NULL;

	memcpy(handle, fid, len);

	dentry = fuse_get_dentry(sb, handle);
	kfree(handle);

	return dentry;
}

static struct dentry *fuse_fh_to_parent(struct super_block *sb,
		struct fid *fid, int fh_len, int fh_type)
{
	switch (fh_type) {
	case FILEID_INO64_GEN:
	case FILEID_INO64_GEN_PARENT:
		return fuse_fh_gen_to_parent(sb, fid, fh_len);
	case FILEID_FUSE_WITHOUT_PARENT:
	case FILEID_FUSE_WITH_PARENT:
		return fuse_fh_fuse_to_parent(sb, fid, fh_len);
	}

	return NULL;
}

static struct dentry *fuse_get_parent(struct dentry *child)
{
	struct inode *child_inode = d_inode(child);
	struct fuse_conn *fc = get_fuse_conn(child_inode);
	struct inode *inode;
	struct dentry *parent;
	struct fuse_entry_out *outarg;
	int err;

	if (!fc->export_support)
		return ERR_PTR(-ESTALE);

	outarg = fuse_entry_out_alloc(fc);
	if (!outarg)
		return ERR_PTR(-ENOMEM);

	err = fuse_lookup_name(child_inode->i_sb, get_node_id(child_inode),
			       child_inode, &dotdot_name, outarg, &inode);
	kfree(outarg);
	if (err) {
		if (err == -ENOENT)
			return ERR_PTR(-ESTALE);
		return ERR_PTR(err);
	}

	parent = d_obtain_alias(inode);
	if (!IS_ERR(parent) && get_node_id(inode) != FUSE_ROOT_ID)
		fuse_invalidate_entry_cache(parent);

	return parent;
}

/* only for fid encoding; no support for file handle */
const struct export_operations fuse_export_fid_operations = {
	.encode_fh	= fuse_encode_fh,
};

const struct export_operations fuse_export_operations = {
	.fh_to_dentry	= fuse_fh_to_dentry,
	.fh_to_parent	= fuse_fh_to_parent,
	.encode_fh	= fuse_encode_fh,
	.get_parent	= fuse_get_parent,
};

