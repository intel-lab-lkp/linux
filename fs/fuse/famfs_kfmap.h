/* SPDX-License-Identifier: GPL-2.0 */
/*
 * famfs - dax file system for shared fabric-attached memory
 *
 * Copyright 2023-2026 Micron Technology, Inc.
 */
#ifndef FAMFS_KFMAP_H
#define FAMFS_KFMAP_H

/*
 * The structures below are the in-memory metadata format for famfs files.
 * Metadata retrieved via the GET_FMAP response is converted to this format
 * for use in resolving file mapping faults.
 *
 * The GET_FMAP response contains the same information, but in a more
 * message-and-versioning-friendly format. Those structs can be found in the
 * famfs section of include/uapi/linux/fuse.h (aka fuse_kernel.h in libfuse)
 */

enum famfs_file_type {
	FAMFS_REG,
	FAMFS_SUPERBLOCK,
	FAMFS_LOG,
};

/* We anticipate the possibility of supporting additional types of extents */
enum famfs_extent_type {
	SIMPLE_DAX_EXTENT,
	INTERLEAVED_EXTENT,
	INVALID_EXTENT_TYPE,
};

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
 * Each famfs dax file has this hanging from its fuse_inode->famfs_meta
 */
struct famfs_file_meta {
	bool                   error;
	enum famfs_file_type   file_type;
	size_t                 file_size;
	enum famfs_extent_type fm_extent_type;
	u64 dev_bitmap; /* bitmap of referenced daxdevs by index */
	union {
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

/*
 * famfs_daxdev - tracking struct for a daxdev within a famfs file system
 *
 * This is the in-memory daxdev metadata that is populated by parsing
 * the responses to GET_FMAP messages
 */
struct famfs_daxdev {
	/* Include dev uuid? */
	bool valid;
	bool error;
	dev_t devno;
	struct dax_device *devp;
	char *name;
};

#define MAX_DAXDEVS 24

/*
 * famfs_dax_devlist - list of famfs_daxdev's
 */
struct famfs_dax_devlist {
	int nslots;
	int ndevs;
	struct famfs_daxdev *devlist;
};

#endif /* FAMFS_KFMAP_H */
