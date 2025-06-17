// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE-BPF: Filesystem in Userspace with BPF
 * Copyright (c) 2021 Google LLC
 */

#include "fuse_i.h"

#include <linux/backing-file.h>

int fuse_open_backing(struct inode *inode, struct file *file, bool isdir)
{
	struct fuse_mount *fm = get_fuse_mount(inode);
	struct fuse_file *ff;
	int retval;
	int mask;
	union fuse_dentry *fd = get_fuse_dentry(file->f_path.dentry);
	struct file *backing_file;
	uint32_t flags = file->f_flags & ~(O_CREAT | O_EXCL | O_NOCTTY);

	ff = fuse_file_alloc(fm, true);
	if (!ff)
		return -ENOMEM;

	switch (flags & O_ACCMODE) {
	case O_RDONLY:
		mask = MAY_READ;
		break;

	case O_WRONLY:
		mask = MAY_WRITE;
		break;

	case O_RDWR:
		mask = MAY_READ | MAY_WRITE;
		break;

	default:
		retval = -EINVAL;
		goto outerr;
	}

	retval = inode_permission(&nop_mnt_idmap,
				  get_fuse_inode(inode)->backing_inode, mask);
	if (retval)
		goto outerr;

	backing_file = backing_file_open(&file->f_path, file->f_flags,
		&fd->backing_path, current_cred());

	if (IS_ERR(backing_file)) {
		retval = PTR_ERR(backing_file);
		goto outerr;
	}

	ff->backing_file = backing_file;
	ff->nodeid = get_fuse_inode(inode)->nodeid;
	file->private_data = ff;
	return 0;

outerr:
	if (retval)
		fuse_file_free(ff);
	return retval;
}

int fuse_handle_backing(struct fuse_entry_backing *feb,
	struct inode **backing_inode, struct path *backing_path)
{
	struct file *backing_file = feb->backing_file;

	if (!backing_file)
		return -EINVAL;
	if (IS_ERR(backing_file))
		return PTR_ERR(backing_file);

	if (backing_inode)
		iput(*backing_inode);
	*backing_inode = backing_file->f_inode;
	ihold(*backing_inode);

	path_put(backing_path);
	*backing_path = backing_file->f_path;
	path_get(backing_path);

	return 0;
}

int fuse_flush_backing(struct file *file, fl_owner_t id)
{
	struct fuse_file *fuse_file = file->private_data;
	struct file *backing_file = fuse_file->backing_file;

	if (backing_file->f_op->flush)
		return backing_file->f_op->flush(backing_file, id);
	return 0;
}
