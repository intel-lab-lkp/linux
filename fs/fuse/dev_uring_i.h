/* SPDX-License-Identifier: GPL-2.0
 *
 * FUSE: Filesystem in Userspace
 * Copyright (c) 2023-2024 DataDirect Networks.
 */

#ifndef _FS_FUSE_DEV_URING_I_H
#define _FS_FUSE_DEV_URING_I_H

#include "fuse_i.h"

#ifdef CONFIG_FUSE_IO_URING

enum fuse_ring_req_state {

	/* ring entry received from userspace and it being processed */
	FRRS_COMMIT,

	/* The ring request waits for a new fuse request */
	FRRS_WAIT,

	/* request is in or on the way to user space */
	FRRS_USERSPACE,
};

/** A fuse ring entry, part of the ring queue */
struct fuse_ring_ent {
	/* userspace buffer */
	struct fuse_ring_req __user *rreq;

	/* the ring queue that owns the request */
	struct fuse_ring_queue *queue;

	struct io_uring_cmd *cmd;

	struct list_head list;

	/*
	 * state the request is currently in
	 * (enum fuse_ring_req_state)
	 */
	unsigned int state;

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
	struct list_head ent_avail_queue;

	/*
	 * entries in the process of being committed or in the process
	 * to be send to userspace
	 */
	struct list_head ent_intermediate_queue;
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

	struct fuse_ring_queue **queues;
};

void fuse_uring_destruct(struct fuse_conn *fc);
int fuse_uring_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags);

#else /* CONFIG_FUSE_IO_URING */

struct fuse_ring;

static inline void fuse_uring_create(struct fuse_conn *fc)
{
}

static inline void fuse_uring_destruct(struct fuse_conn *fc)
{
}

#endif /* CONFIG_FUSE_IO_URING */

#endif /* _FS_FUSE_DEV_URING_I_H */
