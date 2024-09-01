// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE: Filesystem in Userspace
 * Copyright (c) 2023-2024 DataDirect Networks.
 */

#include "fuse_dev_i.h"
#include "fuse_i.h"
#include "dev_uring_i.h"

#include <linux/init.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched/signal.h>
#include <linux/uio.h>
#include <linux/miscdevice.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/pipe_fs_i.h>
#include <linux/swap.h>
#include <linux/splice.h>
#include <linux/sched.h>
#include <linux/io_uring.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/io_uring.h>
#include <linux/io_uring/cmd.h>
#include <linux/topology.h>
#include <linux/io_uring/cmd.h>

static int fuse_ring_ring_ent_unset_userspace(struct fuse_ring_ent *ent)
{
	if (WARN_ON_ONCE(ent->state != FRRS_USERSPACE))
		return -EIO;

	ent->state = FRRS_COMMIT;
	list_del_init(&ent->list);

	return 0;
}

/* Update conn limits according to ring values */
static void fuse_uring_conn_cfg_limits(struct fuse_ring *ring)
{
	struct fuse_conn *fc = ring->fc;

	/*
	 * This not ideal, as multiplication with nr_queue assumes the limit
	 * gets reached when all queues are used, but even a single queue
	 * might reach the limit.
	 */
	WRITE_ONCE(fc->max_background, ring->nr_queues * ring->max_nr_async);
}

static void fuse_uring_queue_cfg(struct fuse_ring_queue *queue, int qid,
				 struct fuse_ring *ring)
{
	int tag;

	queue->qid = qid;
	queue->ring = ring;

	spin_lock_init(&queue->lock);

	INIT_LIST_HEAD(&queue->sync_ent_avail_queue);
	INIT_LIST_HEAD(&queue->async_ent_avail_queue);

	for (tag = 0; tag < ring->queue_depth; tag++) {
		struct fuse_ring_ent *ent = &queue->ring_ent[tag];

		ent->queue = queue;
		ent->tag = tag;

		ent->state = FRRS_INIT;

		INIT_LIST_HEAD(&ent->list);
	}
}

static int _fuse_uring_conn_cfg(struct fuse_ring_config *rcfg,
				struct fuse_conn *fc, struct fuse_ring *ring,
				size_t queue_sz)
{
	ring->numa_aware = rcfg->numa_aware;
	ring->nr_queues = rcfg->nr_queues;
	ring->per_core_queue = rcfg->nr_queues > 1;

	ring->max_nr_sync = rcfg->sync_queue_depth;
	ring->max_nr_async = rcfg->async_queue_depth;
	ring->queue_depth = ring->max_nr_sync + ring->max_nr_async;

	ring->req_buf_sz = rcfg->user_req_buf_sz;

	ring->queue_size = queue_sz;

	fc->ring = ring;
	ring->fc = fc;

	return 0;
}

static int fuse_uring_cfg_sanity(struct fuse_ring_config *rcfg)
{
	if (rcfg->nr_queues == 0) {
		pr_info("zero number of queues is invalid.\n");
		return -EINVAL;
	}

	if (rcfg->nr_queues > 1 && rcfg->nr_queues != num_present_cpus()) {
		pr_info("nr-queues (%d) does not match nr-cores (%d).\n",
			rcfg->nr_queues, num_present_cpus());
		return -EINVAL;
	}

	return 0;
}

/*
 * Basic ring setup for this connection based on the provided configuration
 */
int fuse_uring_conn_cfg(struct file *file, void __user *argp)
{
	struct fuse_ring_config rcfg;
	int res;
	struct fuse_dev *fud;
	struct fuse_conn *fc;
	struct fuse_ring *ring = NULL;
	struct fuse_ring_queue *queue;
	int qid;

	res = copy_from_user(&rcfg, (void *)argp, sizeof(rcfg));
	if (res != 0)
		return -EFAULT;
	res = fuse_uring_cfg_sanity(&rcfg);
	if (res != 0)
		return res;

	fud = fuse_get_dev(file);
	if (fud == NULL)
		return -ENODEV;
	fc = fud->fc;

	if (fc->ring == NULL) {
		size_t queue_depth = rcfg.async_queue_depth +
				     rcfg.sync_queue_depth;
		size_t queue_sz = sizeof(struct fuse_ring_queue) +
				  sizeof(struct fuse_ring_ent) * queue_depth;

		ring = kvzalloc(sizeof(*fc->ring) + queue_sz * rcfg.nr_queues,
				GFP_KERNEL_ACCOUNT);
		if (ring == NULL)
			return -ENOMEM;

		spin_lock(&fc->lock);
		if (fc->ring == NULL)
			res = _fuse_uring_conn_cfg(&rcfg, fc, ring, queue_sz);
		else
			res = -EALREADY;
		spin_unlock(&fc->lock);
		if (res != 0)
			goto err;
	}

	for (qid = 0; qid < ring->nr_queues; qid++) {
		queue = fuse_uring_get_queue(ring, qid);
		fuse_uring_queue_cfg(queue, qid, ring);
	}

	return 0;
err:
	kvfree(ring);
	return res;
}

/*
 * Put a ring request onto hold, it is no longer used for now.
 */
static void fuse_uring_ent_avail(struct fuse_ring_ent *ring_ent,
				 struct fuse_ring_queue *queue)
	__must_hold(&queue->lock)
{
	struct fuse_ring *ring = queue->ring;

	lockdep_assert_held(&queue->lock);

	/* unsets all previous flags - basically resets */
	pr_devel("%s ring=%p qid=%d tag=%d state=%d async=%d\n", __func__,
		 ring, ring_ent->queue->qid, ring_ent->tag, ring_ent->state,
		 ring_ent->async);

	if (WARN_ON(ring_ent->state != FRRS_COMMIT)) {
		pr_warn("%s qid=%d tag=%d state=%d async=%d\n", __func__,
			ring_ent->queue->qid, ring_ent->tag, ring_ent->state,
			ring_ent->async);
		return;
	}

	WARN_ON_ONCE(!list_empty(&ring_ent->list));

	if (ring_ent->async)
		list_add(&ring_ent->list, &queue->async_ent_avail_queue);
	else
		list_add(&ring_ent->list, &queue->sync_ent_avail_queue);

	ring_ent->state = FRRS_WAIT;
}

/*
 * fuse_uring_req_fetch command handling
 */
static int _fuse_uring_fetch(struct fuse_ring_ent *ring_ent,
			    struct io_uring_cmd *cmd, unsigned int issue_flags)
__must_hold(ring_ent->queue->lock)
{
	struct fuse_ring_queue *queue = ring_ent->queue;
	struct fuse_ring *ring = queue->ring;
	int nr_ring_sqe;

	lockdep_assert_held(&queue->lock);

	/* register requests for foreground requests first, then backgrounds */
	if (queue->nr_req_sync >= ring->max_nr_sync) {
		queue->nr_req_async++;
		ring_ent->async = 1;
	} else
		queue->nr_req_sync++;

	fuse_uring_ent_avail(ring_ent, queue);

	if (WARN_ON_ONCE(queue->nr_req_sync +
			 queue->nr_req_async > ring->queue_depth)) {
		/* should be caught by ring state before and queue depth
		 * check before
		 */
		pr_info("qid=%d tag=%d req cnt (fg=%d async=%d exceeds depth=%zu",
			queue->qid, ring_ent->tag, queue->nr_req_sync,
			queue->nr_req_async, ring->queue_depth);
		return -ERANGE;
	}

	WRITE_ONCE(ring_ent->cmd, cmd);

	nr_ring_sqe = ring->queue_depth * ring->nr_queues;
	if (atomic_inc_return(&ring->nr_sqe_init) == nr_ring_sqe) {
		fuse_uring_conn_cfg_limits(ring);
		ring->ready = 1;
	}

	return 0;
}

static int fuse_uring_fetch(struct fuse_ring_ent *ring_ent,
			    struct io_uring_cmd *cmd, unsigned int issue_flags)
	__releases(ring_ent->queue->lock)
{
	struct fuse_ring *ring = ring_ent->queue->ring;
	struct fuse_ring_queue *queue = ring_ent->queue;
	int ret;

	/* No other bit must be set here */
	ret = -EINVAL;
	if (ring_ent->state != FRRS_INIT)
		goto err;

	/*
	 * FUSE_URING_REQ_FETCH is an initialization exception, needs
	 * state override
	 */
	ring_ent->state = FRRS_USERSPACE;
	ret = fuse_ring_ring_ent_unset_userspace(ring_ent);
	if (ret != 0) {
		pr_info_ratelimited(
			"qid=%d tag=%d register req state %d expected %d",
			queue->qid, ring_ent->tag, ring_ent->state,
			FRRS_INIT);
		goto err;
	}

	ret = _fuse_uring_fetch(ring_ent, cmd, issue_flags);
	if (ret)
		goto err;

	/*
	 * The ring entry is registered now and needs to be handled
	 * for shutdown.
	 */
	atomic_inc(&ring->queue_refs);
err:
	spin_unlock(&queue->lock);
	return ret;
}

/**
 * Entry function from io_uring to handle the given passthrough command
 * (op cocde IORING_OP_URING_CMD)
 */
int fuse_uring_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
	const struct fuse_uring_cmd_req *cmd_req = io_uring_sqe_cmd(cmd->sqe);
	struct fuse_dev *fud;
	struct fuse_conn *fc;
	struct fuse_ring *ring;
	struct fuse_ring_queue *queue;
	struct fuse_ring_ent *ring_ent = NULL;
	u32 cmd_op = cmd->cmd_op;
	int ret = 0;

	ret = -ENODEV;
	fud = fuse_get_dev(cmd->file);
	if (!fud)
		goto out;
	fc = fud->fc;

	ring = fc->ring;
	if (!ring)
		goto out;

	queue = fud->ring_q;
	if (!queue)
		goto out;

	ret = -EINVAL;
	if (queue->qid != cmd_req->qid)
		goto out;

	ret = -ERANGE;
	if (cmd_req->tag > ring->queue_depth)
		goto out;

	ring_ent = &queue->ring_ent[cmd_req->tag];

	pr_devel("%s:%d received: cmd op %d qid %d (%p) tag %d  (%p)\n",
		 __func__, __LINE__, cmd_op, cmd_req->qid, queue, cmd_req->tag,
		 ring_ent);

	spin_lock(&queue->lock);
	ret = -ENOTCONN;
	if (unlikely(fc->aborted || queue->stopped))
		goto err_unlock;

	switch (cmd_op) {
	case FUSE_URING_REQ_FETCH:
		ret = fuse_uring_fetch(ring_ent, cmd, issue_flags);
		break;
	default:
		ret = -EINVAL;
		pr_devel("Unknown uring command %d", cmd_op);
		goto err_unlock;
	}
out:
	pr_devel("uring cmd op=%d, qid=%d tag=%d ret=%d\n", cmd_op,
		 cmd_req->qid, cmd_req->tag, ret);

	if (ret < 0) {
		if (ring_ent != NULL) {
			pr_info_ratelimited("error: uring cmd op=%d, qid=%d tag=%d ret=%d\n",
					    cmd_op, cmd_req->qid, cmd_req->tag,
					    ret);

			/* must not change the entry state, as userspace
			 * might have sent random data, but valid requests
			 * might be registered already - don't confuse those.
			 */
		}
		io_uring_cmd_done(cmd, ret, 0, issue_flags);
	}

	return -EIOCBQUEUED;

err_unlock:
	spin_unlock(&queue->lock);
	goto out;
}
