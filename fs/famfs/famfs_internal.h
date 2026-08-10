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
#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/build_bug.h>

#include <linux/famfs_ioctl.h>

/*
 * Default operation-permission bitmap (see FAMFS_OPT_* in the uapi header).
 * This preserves famfs's historical behavior: file/dir creation, the fmap
 * ioctl, data writes, and the non-resize setattr components are permitted;
 * unlink of mapped files, link, symlink, mknod, rmdir, rename and truncate
 * are denied until enabled via FAMFSIOC_SET_OPTS.
 */
#define FAMFS_OPT_DEFAULT	(FAMFS_OPT_CREATE | FAMFS_OPT_MKDIR | \
				 FAMFS_OPT_CHMOD | FAMFS_OPT_CHOWN | \
				 FAMFS_OPT_UTIMES | FAMFS_OPT_WRITE | \
				 FAMFS_OPT_MAP_CREATE)

extern const struct file_operations famfs_file_operations;

/*
 * Internal sanity bound on a FAMFSIOC_MAP_CREATE fmap message. The ABI does
 * not advertise a maximum (the message is self-describing); this only guards
 * the copy-in against an unreasonable allocation. Oversize is rejected with
 * -EFBIG.
 */
#define FAMFS_FMAP_MSG_MAX (4 * 1024 * 1024)

struct famfs_meta_simple_ext {
	u64 dev_index;
	u64 ext_offset;
	u64 ext_len;
};

struct famfs_meta_interleaved_ext {
	u64 fie_nstrips;
	u64 fie_chunk_size;
	u64 fie_nbytes;
	struct famfs_meta_simple_ext *ie_strips;
};

/*
 * Each famfs dax file has this hanging from its inode->i_private.
 */
struct famfs_file_meta {
	bool                   error;
	enum famfs_file_type   file_type;
	size_t                 file_size;
	enum famfs_ioc_ext_type fm_extent_type;
	u64                    dev_bitmap; /* referenced daxdev indices */
	union { /* This will make code a bit more readable */
		struct {
			size_t         fm_nextents;
			struct famfs_meta_simple_ext  *se;
		};
		struct {
			size_t         fm_niext;
			struct famfs_meta_interleaved_ext *ie;
		};
	};
};

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
 * @opts:        Operation-permission bitmap (FAMFS_OPT_*), adjusted at runtime
 *               via the FAMFSIOC_{GET,SET,CLEAR}_OPTS ioctls
 * @deverror:    True if the dax device has called our notify_failure entry
 *               point, or if other "shutdown" conditions exist
 * @dax_devlist: Table of backing daxdevs (slot 0 is the mount primary)
 * @devlist_sem: Serializes installs into, and teardown of, @dax_devlist
 */
struct famfs_fs_info {
	struct famfs_mount_opts   mount_opts;
	atomic64_t                opts;
	bool                      deverror;
	struct famfs_dax_devlist *dax_devlist;
	struct rw_semaphore       devlist_sem;
};

/*
 * famfs_opt_enabled() - is operation permission @opt enabled for this mount?
 *
 * @opt is a single FAMFS_OPT_* bit; returns true if that operation is
 * permitted. The bitmap is read locklessly (updated via atomic RMW by the
 * FAMFSIOC_{SET,CLEAR}_OPTS ioctls).
 */
static inline bool famfs_opt_enabled(struct famfs_fs_info *fsi, u64 opt)
{
	return !!(atomic64_read(&fsi->opts) & opt);
}

int famfs_lookup_daxdev(const char *pathname, dev_t *devno);
int famfs_devlist_alloc(struct famfs_fs_info *fsi);
int famfs_install_daxdev(struct famfs_fs_info *fsi, struct super_block *sb,
			 u64 index, dev_t devno, const char *name);

void famfs_meta_free(struct famfs_file_meta *map);

#endif /* FAMFS_INTERNAL_H */
