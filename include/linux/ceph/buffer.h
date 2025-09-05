/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __FS_CEPH_BUFFER_H
#define __FS_CEPH_BUFFER_H

#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/types.h>
#include <linux/uio.h>

/*
 * Reference counted buffer metadata: Simple buffer management with automatic
 * memory allocation strategy. Uses kmalloc for smaller buffers and vmalloc
 * for larger buffers to optimize memory usage and fragmentation.
 */
struct ceph_buffer {
	/* Reference counting for safe shared access */
	struct kref kref;
	/* Kernel vector containing buffer pointer and length */
	struct kvec vec;
	/* Total allocated buffer size (may be larger than vec.iov_len) */
	size_t alloc_len;
};

extern struct ceph_buffer *ceph_buffer_new(size_t len, gfp_t gfp);
extern void ceph_buffer_release(struct kref *kref);

static inline struct ceph_buffer *ceph_buffer_get(struct ceph_buffer *b)
{
	kref_get(&b->kref);
	return b;
}

static inline void ceph_buffer_put(struct ceph_buffer *b)
{
	if (b)
		kref_put(&b->kref, ceph_buffer_release);
}

extern int ceph_decode_buffer(struct ceph_buffer **b, void **p, void *end);

#endif
