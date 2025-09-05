/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CEPH_STRIPER_H
#define _LINUX_CEPH_STRIPER_H

#include <linux/list.h>
#include <linux/types.h>

struct ceph_file_layout;

void ceph_calc_file_object_mapping(struct ceph_file_layout *l,
				   u64 off, u64 len,
				   u64 *objno, u64 *objoff, u32 *xlen);

/*
 * Object extent metadata: Represents a contiguous range within a RADOS object
 * that maps to part of a file. Used by the striping layer to break file I/O
 * operations into object-level operations distributed across the cluster.
 */
struct ceph_object_extent {
	/* Linkage for lists of extents */
	struct list_head oe_item;
	/* RADOS object number containing this extent */
	u64 oe_objno;
	/* Byte offset within the object */
	u64 oe_off;
	/* Length of the extent in bytes */
	u64 oe_len;
};

static inline void ceph_object_extent_init(struct ceph_object_extent *ex)
{
	INIT_LIST_HEAD(&ex->oe_item);
}

/*
 * Called for each mapped stripe unit.
 *
 * @bytes: number of bytes mapped, i.e. the minimum of the full length
 *         requested (file extent length) or the remainder of the stripe
 *         unit within an object
 */
typedef void (*ceph_object_extent_fn_t)(struct ceph_object_extent *ex,
					u32 bytes, void *arg);

int ceph_file_to_extents(struct ceph_file_layout *l, u64 off, u64 len,
			 struct list_head *object_extents,
			 struct ceph_object_extent *alloc_fn(void *arg),
			 void *alloc_arg,
			 ceph_object_extent_fn_t action_fn,
			 void *action_arg);
int ceph_iterate_extents(struct ceph_file_layout *l, u64 off, u64 len,
			 struct list_head *object_extents,
			 ceph_object_extent_fn_t action_fn,
			 void *action_arg);

/*
 * File extent metadata: Represents a contiguous range within a file.
 * Used to describe logical file ranges that correspond to object extents
 * after stripe mapping calculations.
 */
struct ceph_file_extent {
	/* Byte offset within the file */
	u64 fe_off;
	/* Length of the extent in bytes */
	u64 fe_len;
};

static inline u64 ceph_file_extents_bytes(struct ceph_file_extent *file_extents,
					  u32 num_file_extents)
{
	u64 bytes = 0;
	u32 i;

	for (i = 0; i < num_file_extents; i++)
		bytes += file_extents[i].fe_len;

	return bytes;
}

int ceph_extent_to_file(struct ceph_file_layout *l,
			u64 objno, u64 objoff, u64 objlen,
			struct ceph_file_extent **file_extents,
			u32 *num_file_extents);

u64 ceph_get_num_objects(struct ceph_file_layout *l, u64 size);

#endif
