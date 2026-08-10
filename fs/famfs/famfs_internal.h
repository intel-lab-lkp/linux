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

#include <linux/rwsem.h>
#include <linux/bits.h>
#include <linux/build_bug.h>

extern const struct file_operations famfs_file_operations;

struct famfs_mount_opts {
	umode_t mode;
};

/*
 * famfs_daxdev - one entry in the per-superblock daxdev table
 *
 * @valid:   slot is populated and the daxdev has been exclusively acquired
 * @error:   dax reported a memory error (probably poison) via notify_failure
 * @dax_err: fs_dax_get() failed for this daxdev
 * @devno:   dax device dev_t
 * @devp:    the acquired dax_device
 * @name:    dax device path (may be NULL for ioctl-registered daxdevs)
 */
struct famfs_daxdev {
	bool valid;
	bool error;
	bool dax_err;
	dev_t devno;
	struct dax_device *devp;
	char *name;
};

/*
 * The daxdev index space (and thus this table) is capped at 64 so the set of
 * daxdev indices referenced by a file's fmap fits in a u64 bitmap.
 */
#define FAMFS_MAX_DAXDEVS 64
static_assert(BITS_PER_TYPE(u64) >= FAMFS_MAX_DAXDEVS);

/*
 * famfs_dax_devlist - the per-superblock table of famfs_daxdev's. Slot 0 is
 * the primary daxdev supplied at mount; slots 1..n are registered via ioctl.
 */
struct famfs_dax_devlist {
	int nslots;
	struct famfs_daxdev *devlist;
};

/**
 * @famfs_fs_info
 *
 * @mount_opts:  The mount options
 * @deverror:    True if the dax device has called our notify_failure entry
 *               point, or if other "shutdown" conditions exist
 * @dax_devlist: Table of backing daxdevs (slot 0 is the mount primary)
 * @devlist_sem: Serializes installs into, and teardown of, @dax_devlist
 */
struct famfs_fs_info {
	struct famfs_mount_opts   mount_opts;
	bool                      deverror;
	struct famfs_dax_devlist *dax_devlist;
	struct rw_semaphore       devlist_sem;
};

/* This stub will be replaced in a later commit
 * Note: the opt parameter is intentionally unused, and will be used by
 * the replacement function when that commit lands
 */
#define famfs_opt_enabled(fsi, opt) (fsi != 0)

int famfs_lookup_daxdev(const char *pathname, dev_t *devno);
int famfs_devlist_alloc(struct famfs_fs_info *fsi);
int famfs_install_daxdev(struct famfs_fs_info *fsi, struct super_block *sb,
			 u64 index, dev_t devno, const char *name);

#endif /* FAMFS_INTERNAL_H */
