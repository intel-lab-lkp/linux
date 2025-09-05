/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FS_CEPH_HASH_H
#define FS_CEPH_HASH_H

/*
 * String hashing algorithm type constants used for Ceph directory layout hashing.
 * These determine how directory entries are distributed across metadata servers.
 */
/* Linux dcache hash algorithm - matches kernel dcache string hashing */
#define CEPH_STR_HASH_LINUX      0x1
/* Robert Jenkins' hash algorithm - provides good distribution properties */
#define CEPH_STR_HASH_RJENKINS   0x2

/*
 * String hashing function interfaces for Ceph filesystem operations.
 * Used primarily for consistent directory entry placement across MDS nodes.
 */

/* Compute Linux dcache-style hash of a string */
extern unsigned ceph_str_hash_linux(const char *s, unsigned len);

/* Compute Robert Jenkins hash of a string */
extern unsigned ceph_str_hash_rjenkins(const char *s, unsigned len);

/* Generic hash function dispatcher - calls appropriate algorithm based on type */
extern unsigned ceph_str_hash(int type, const char *s, unsigned len);

/* Get human-readable name for a hash algorithm type */
extern const char *ceph_str_hash_name(int type);

#endif
