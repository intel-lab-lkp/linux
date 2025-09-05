/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FS_CEPH_MSGPOOL
#define _FS_CEPH_MSGPOOL

#include <linux/mempool.h>

/*
 * Ceph message pool metadata: Memory pool for preallocating network messages
 * to avoid out-of-memory conditions during critical operations. Maintains
 * a reserve of messages with specific types and sizes for reliable operation
 * under memory pressure.
 */
struct ceph_msgpool {
	/* Descriptive name for debugging and identification */
	const char *name;
	/* Underlying kernel memory pool */
	mempool_t *pool;
	/* Message type for preallocated messages */
	int type;
	/* Size of preallocated front payload */
	int front_len;
	/* Maximum number of data items in preallocated messages */
	int max_data_items;
};

int ceph_msgpool_init(struct ceph_msgpool *pool, int type,
		      int front_len, int max_data_items, int size,
		      const char *name);
extern void ceph_msgpool_destroy(struct ceph_msgpool *pool);
struct ceph_msg *ceph_msgpool_get(struct ceph_msgpool *pool, int front_len,
				  int max_data_items);
extern void ceph_msgpool_put(struct ceph_msgpool *, struct ceph_msg *);

#endif
