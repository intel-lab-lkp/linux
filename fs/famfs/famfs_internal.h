/* SPDX-License-Identifier: GPL-2.0 */
/*
 * famfs - dax file system for shared fabric-attached memory
 *
 * Copyright 2023-2024 Micron Technology, Inc.
 *
 * This file system, originally based on ramfs the dax support from xfs,
 * is intended to allow multiple host systems to mount a common file system
 * view of dax files that map to shared memory.
 */
#ifndef FAMFS_INTERNAL_H
#define FAMFS_INTERNAL_H

struct famfs_mount_opts {
	umode_t mode;
};

/**
 * @famfs_fs_info
 *
 * @mount_opts:  The mount options
 * @deverror:    True if the dax device has called our notify_failure entry
 *               point, or if other "shutdown" conditions exist
 */
struct famfs_fs_info {
	struct famfs_mount_opts   mount_opts;
	bool                      deverror;
};

int famfs_lookup_daxdev(const char *pathname, dev_t *devno);

#endif /* FAMFS_INTERNAL_H */
