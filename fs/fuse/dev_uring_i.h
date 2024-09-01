/* SPDX-License-Identifier: GPL-2.0
 *
 * FUSE: Filesystem in Userspace
 * Copyright (c) 2023-2024 DataDirect Networks.
 */

#ifndef _FS_FUSE_DEV_URING_I_H
#define _FS_FUSE_DEV_URING_I_H

#include "fuse_i.h"

#ifdef CONFIG_FUSE_IO_URING

/* IORING_MAX_ENTRIES */
#define FUSE_URING_MAX_QUEUE_DEPTH 32768

#define FUSE_URING_TEARDOWN_TIMEOUT (5 * HZ)
#define FUSE_URING_TEARDOWN_INTERVAL (HZ/20)

enum fuse_ring_req_state {
	FRRS_INVALID = 0,

	/* request is basially initialized */
	FRRS_INIT,

	/* ring entry received from userspace and it being processed */
	FRRS_COMMIT,

	/* The ring request waits for a new fuse request */
	FRRS_WAIT,

	/* The ring req got assigned a fuse req */
	FRRS_FUSE_REQ,

	/* request is in or on the way to user space */
	FRRS_USERSPACE,

	/* request is released */
	FRRS_FREED,
};

/* A fuse ring entry, part of the ring queue */
struct fuse_ring_ent {
	/* back pointer */
	struct fuse_ring_queue *queue;

	/* array index in the ring-queue */
	unsigned int tag;

	/* state the request is currently in */
	enum fuse_ring_req_state state;

	/* is this an async or sync entry */
	unsigned int async : 1;

	struct list_head list;

	struct io_uring_cmd *cmd;

	/* fuse_req assigned to the ring entry */
	struct fuse_req *fuse_req;

	/* buffer provided by fuse server */
	unsigned long __user user_buf;

	/* length of user_buf */
	size_t user_buf_len;

	/* mapped user_buf pages */
	struct page **user_pages;

	/* number of user pages */
	int nr_user_pages;

	/* struct fuse_ring_req::in_out_arg size*/
	size_t max_arg_len;
};

struct fuse_ring_queue {
	/*
	 * back pointer to the main fuse uring structure that holds this
	 * queue
	 */
	struct fuse_ring *ring;

	/* queue id, typically also corresponds to the cpu core */
	unsigned int qid;

	/*
	 * queue lock, taken when any value in the queue changes _and_ also
	 * a ring entry state changes.
	 */
	spinlock_t lock;

	/* available ring entries (struct fuse_ring_ent) */
	struct list_head async_ent_avail_queue;
	struct list_head sync_ent_avail_queue;

	/* fuse fg/bg request types */
	struct list_head async_fuse_req_queue;
	struct list_head sync_fuse_req_queue;

	/* entries sent to userspace */
	struct list_head ent_in_userspace;

	/*
	 * available number of sync requests,
	 * loosely bound to fuse foreground requests
	 */
	int nr_req_sync;

	/*
	 * available number of async requests
	 * loosely bound to fuse background requests
	 */
	int nr_req_async;

	unsigned int stopped : 1;

	/* size depends on queue depth */
	struct fuse_ring_ent ring_ent[] ____cacheline_aligned_in_smp;
};

/**
 * Describes if uring is for communication and holds alls the data needed
 * for uring communication
 */
struct fuse_ring {
	/* back pointer */
	struct fuse_conn *fc;

	/* number of ring queues */
	size_t nr_queues;

	/* number of entries per queue */
	size_t queue_depth;

	/* req_arg_len + sizeof(struct fuse_req) */
	size_t req_buf_sz;

	/* max number of background requests per queue */
	size_t max_nr_async;

	/* max number of foreground requests */
	size_t max_nr_sync;

	/* size of struct fuse_ring_queue + queue-depth * entry-size */
	size_t queue_size;

	/* one queue per core or a single queue only ? */
	unsigned int per_core_queue : 1;

	/* numa aware memory allocation */
	unsigned int numa_aware : 1;

	/* Is the ring read to take requests */
	unsigned int ready : 1;

	/*
	 * Log ring entry states onces on stop when entries cannot be
	 * released
	 */
	unsigned int stop_debug_log : 1;

	/* number of SQEs initialized */
	atomic_t nr_sqe_init;

	/* Used to release the ring on stop */
	atomic_t queue_refs;

	wait_queue_head_t stop_waitq;

	/* async tear down */
	struct delayed_work async_teardown_work;

	/* log */
	unsigned long teardown_time;

	struct fuse_ring_queue queues[] ____cacheline_aligned_in_smp;
};

void fuse_uring_abort_end_requests(struct fuse_ring *ring);
int fuse_uring_conn_cfg(struct file *file, void __user *argp);
void fuse_uring_stop_queues(struct fuse_ring *ring);
int fuse_uring_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags);
int fuse_uring_queue_fuse_req(struct fuse_conn *fc, struct fuse_req *req);

static inline void fuse_uring_conn_destruct(struct fuse_conn *fc)
{
	if (fc->ring == NULL)
		return;

	kvfree(fc->ring);
	fc->ring = NULL;
}

static inline struct fuse_ring_queue *
fuse_uring_get_queue(struct fuse_ring *ring, int qid)
{
	char *ptr = (char *)ring->queues;

	if (WARN_ON(qid > ring->nr_queues))
		qid = 0;

	return (struct fuse_ring_queue *)(ptr + qid * ring->queue_size);
}

static inline bool fuse_uring_configured(struct fuse_conn *fc)
{
	if (fc->ring != NULL)
		return true;

	return false;
}

static inline bool fuse_per_core_queue(struct fuse_conn *fc)
{
	return fc->ring && fc->ring->per_core_queue;
}

static inline void fuse_uring_set_stopped_queues(struct fuse_ring *ring)
{
	int qid;

	for (qid = 0; qid < ring->nr_queues; qid++) {
		struct fuse_ring_queue *queue = fuse_uring_get_queue(ring, qid);

		spin_lock(&queue->lock);
		queue->stopped = 1;
		spin_unlock(&queue->lock);
	}
}

/*
 *  Set per queue aborted flag
 */
static inline void fuse_uring_set_stopped(struct fuse_conn *fc)
	__must_hold(fc->lock)
{
	if (fc->ring == NULL)
		return;

	fc->ring->ready = false;

	fuse_uring_set_stopped_queues(fc->ring);
}

static inline void fuse_uring_abort(struct fuse_conn *fc)
{
	struct fuse_ring *ring = fc->ring;

	if (ring == NULL)
		return;

	if (atomic_read(&ring->queue_refs) > 0) {
		fuse_uring_abort_end_requests(ring);
		fuse_uring_stop_queues(ring);
	}
}

static inline void fuse_uring_wait_stopped_queues(struct fuse_conn *fc)
{
	struct fuse_ring *ring = fc->ring;

	if (ring)
		wait_event(ring->stop_waitq,
			   atomic_read(&ring->queue_refs) == 0);
}

static inline bool fuse_uring_ready(struct fuse_conn *fc)
{
	return fc->ring && fc->ring->ready;
}

#else /* CONFIG_FUSE_IO_URING */

struct fuse_ring;

static inline void fuse_uring_conn_init(struct fuse_ring *ring,
					struct fuse_conn *fc)
{
}

static inline void fuse_uring_conn_destruct(struct fuse_conn *fc)
{
}

static inline bool fuse_uring_configured(struct fuse_conn *fc)
{
	return false;
}

static inline bool fuse_per_core_queue(struct fuse_conn *fc)
{
	return false;
}

static inline void fuse_uring_set_stopped(struct fuse_conn *fc)
{
}

static inline void fuse_uring_abort(struct fuse_conn *fc)
{
}

static inline void fuse_uring_wait_stopped_queues(struct fuse_conn *fc)
{
}

static inline bool fuse_uring_ready(struct fuse_conn *fc)
{
	return false;
}

static inline int
fuse_uring_queue_fuse_req(struct fuse_conn *fc, struct fuse_req *req)
{
	return -EPFNOSUPPORT;
}
#endif /* CONFIG_FUSE_IO_URING */

#endif /* _FS_FUSE_DEV_URING_I_H */
