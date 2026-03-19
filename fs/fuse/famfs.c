// SPDX-License-Identifier: GPL-2.0
/*
 * famfs - dax file system for shared fabric-attached memory
 *
 * Copyright 2023-2026 Micron Technology, Inc.
 *
 * This file system, originally based on ramfs the dax support from xfs,
 * is intended to allow multiple host systems to mount a common file system
 * view of dax files that map to shared memory.
 */

#include <linux/cleanup.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/dax.h>
#include <linux/iomap.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/string.h>

#include "fuse_i.h"


#define FMAP_BUFSIZE PAGE_SIZE

int fuse_get_fmap(struct fuse_mount *fm, struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	size_t fmap_bufsize = FMAP_BUFSIZE;
	u64 nodeid = get_node_id(inode);
	ssize_t fmap_size;
	int rc;

	FUSE_ARGS(args);

	/* Don't retrieve if we already have the famfs metadata */
	if (fi->famfs_meta)
		return 0;

	void *fmap_buf __free(kfree) = kzalloc(FMAP_BUFSIZE, GFP_KERNEL);

	if (!fmap_buf)
		return -ENOMEM;

	args.opcode = FUSE_GET_FMAP;
	args.nodeid = nodeid;

	/* Variable-sized output buffer
	 * this causes fuse_simple_request() to return the size of the
	 * output payload
	 */
	args.out_argvar = true;
	args.out_numargs = 1;
	args.out_args[0].size = fmap_bufsize;
	args.out_args[0].value = fmap_buf;

	/* Send GET_FMAP command */
	rc = fuse_simple_request(fm, &args);
	if (rc < 0) {
		pr_err("%s: err=%d from fuse_simple_request()\n",
		       __func__, rc);
		return rc;
	}
	fmap_size = rc;

	/* We retrieved the "fmap" (the file's map to memory), but
	 * we haven't used it yet. A call to famfs_file_init_dax() will be added
	 * here in a subsequent patch, when we add the ability to attach
	 * fmaps to files.
	 */

	return 0;
}
