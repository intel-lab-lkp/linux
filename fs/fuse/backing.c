// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE-BPF: Filesystem in Userspace with BPF
 * Copyright (c) 2021 Google LLC
 */

#include "fuse_i.h"

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
