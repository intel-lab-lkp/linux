/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FS_CEPH_TYPES_H
#define _FS_CEPH_TYPES_H

/* needed before including ceph_fs.h */
#include <linux/in.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/string.h>

#include <linux/ceph/ceph_fs.h>
#include <linux/ceph/ceph_frag.h>
#include <linux/ceph/ceph_hash.h>

/*
 * Virtual inode identifier metadata: Uniquely identifies an inode within
 * the CephFS namespace by combining the inode number with a snapshot ID.
 * This allows the same inode to exist in multiple snapshots simultaneously.
 */
struct ceph_vino {
	/* Inode number within the filesystem */
	u64 ino;
	/* Snapshot ID (CEPH_NOSNAP for head/live version) */
	u64 snap;
};


/*
 * Capability reservation context metadata: Tracks reserved capabilities
 * for atomic operations that require multiple caps. Prevents deadlocks
 * by pre-reserving the required capabilities before starting operations.
 */
struct ceph_cap_reservation {
	/* Total number of capabilities reserved */
	int count;
	/* Number of reserved capabilities already consumed */
	int used;
};


#endif
