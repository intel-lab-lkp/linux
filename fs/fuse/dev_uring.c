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

struct fuse_uring_cmd_pdu {
	struct fuse_ring_ent *ring_ent;
};

/*
 * Finalize a fuse request, then fetch and send the next entry, if available
 */
static void fuse_uring_req_end(struct fuse_ring_ent *ring_ent,
			       bool set_err, int error)
{
	struct fuse_req *req = ring_ent->fuse_req;

	if (set_err)
		req->out.h.error = error;

	clear_bit(FR_SENT, &req->flags);
	fuse_request_end(ring_ent->fuse_req);
	ring_ent->fuse_req = NULL;
}

static int fuse_ring_ring_ent_unset_userspace(struct fuse_ring_ent *ent)
{
	if (WARN_ON_ONCE(ent->state != FRRS_USERSPACE))
		return -EIO;

	ent->state = FRRS_COMMIT;
	list_del_init(&ent->list);

	return 0;
}

static void
fuse_uring_async_send_to_ring(struct io_uring_cmd *cmd,
			      unsigned int issue_flags)
{
	io_uring_cmd_done(cmd, 0, 0, issue_flags);
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
	INIT_LIST_HEAD(&queue->ent_in_userspace);
	INIT_LIST_HEAD(&queue->sync_fuse_req_queue);
	INIT_LIST_HEAD(&queue->async_fuse_req_queue);

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
 * Checks for errors and stores it into the request
 */
static int fuse_uring_out_header_has_err(struct fuse_out_header *oh,
					 struct fuse_req *req,
					 struct fuse_conn *fc)
{
	int err;

	if (oh->unique == 0) {
		/* Not supportd through request based uring, this needs another
		 * ring from user space to kernel
		 */
		pr_warn("Unsupported fuse-notify\n");
		err = -EINVAL;
		goto seterr;
	}

	if (oh->error <= -512 || oh->error > 0) {
		err = -EINVAL;
		goto seterr;
	}

	if (oh->error) {
		err = oh->error;
		pr_devel("%s:%d err=%d op=%d req-ret=%d", __func__, __LINE__,
			 err, req->args->opcode, req->out.h.error);
		goto err; /* error already set */
	}

	if ((oh->unique & ~FUSE_INT_REQ_BIT) != req->in.h.unique) {
		pr_warn("Unpexted seqno mismatch, expected: %llu got %llu\n",
			req->in.h.unique, oh->unique & ~FUSE_INT_REQ_BIT);
		err = -ENOENT;
		goto seterr;
	}

	/* Is it an interrupt reply ID?	 */
	if (oh->unique & FUSE_INT_REQ_BIT) {
		err = 0;
		if (oh->error == -ENOSYS)
			fc->no_interrupt = 1;
		else if (oh->error == -EAGAIN) {
			/* XXX Interrupts not handled yet */
			/* err = queue_interrupt(req); */
			pr_warn("Intrerupt EAGAIN not supported yet");
			err = -EINVAL;
		}

		goto seterr;
	}

	return 0;

seterr:
	pr_devel("%s:%d err=%d op=%d req-ret=%d", __func__, __LINE__, err,
		 req->args->opcode, req->out.h.error);
	oh->error = err;
err:
	pr_devel("%s:%d err=%d op=%d req-ret=%d", __func__, __LINE__, err,
		 req->args->opcode, req->out.h.error);
	return err;
}

static int fuse_uring_copy_from_ring(struct fuse_ring *ring,
				     struct fuse_req *req,
				     struct fuse_ring_ent *ent)
{
	struct fuse_ring_req __user *rreq = ent->rreq;
	struct fuse_copy_state cs;
	struct fuse_args *args = req->args;
	struct iov_iter iter;
	int err;
	int res_arg_len;

	err = copy_from_user(&res_arg_len, &rreq->in_out_arg_len,
			     sizeof(res_arg_len));
	if (err)
		return err;

	err = import_ubuf(ITER_SOURCE, (void __user *)&rreq->in_out_arg,
			  ent->max_arg_len, &iter);
	if (err)
		return err;

	fuse_copy_init(&cs, 0, &iter);
	cs.is_uring = 1;
	cs.req = req;

	return fuse_copy_out_args(&cs, args, res_arg_len);
}

 /*
  * Copy data from the req to the ring buffer
  */
static int fuse_uring_copy_to_ring(struct fuse_ring *ring, struct fuse_req *req,
				   struct fuse_ring_ent *ent)
{
	struct fuse_ring_req __user *rreq = ent->rreq;
	struct fuse_copy_state cs;
	struct fuse_args *args = req->args;
	int err, res;
	struct iov_iter iter;

	err = import_ubuf(ITER_DEST, (void __user *)&rreq->in_out_arg,
			  ent->max_arg_len, &iter);
	if (err) {
		pr_info("Import user buffer failed\n");
		return err;
	}

	fuse_copy_init(&cs, 1, &iter);
	cs.is_uring = 1;
	cs.req = req;
	err = fuse_copy_args(&cs, args->in_numargs, args->in_pages,
			     (struct fuse_arg *)args->in_args, 0);
	if (err) {
		pr_info("%s fuse_copy_args failed\n", __func__);
		return err;
	}

	BUILD_BUG_ON((sizeof(rreq->in_out_arg_len) != sizeof(cs.ring.offset)));
	res = copy_to_user(&rreq->in_out_arg_len, &cs.ring.offset,
			   sizeof(rreq->in_out_arg_len));
	err = res > 0 ? -EFAULT : res;

	return err;
}

static int
fuse_uring_prepare_send(struct fuse_ring_ent *ring_ent)
{
	struct fuse_ring_req *rreq = ring_ent->rreq;
	struct fuse_ring_queue *queue = ring_ent->queue;
	struct fuse_ring *ring = queue->ring;
	struct fuse_req *req = ring_ent->fuse_req;
	int err = 0, res;

	if (WARN_ON(ring_ent->state != FRRS_FUSE_REQ)) {
		pr_err("qid=%d tag=%d ring-req=%p buf_req=%p invalid state %d on send\n",
		       queue->qid, ring_ent->tag, ring_ent, rreq,
		       ring_ent->state);
		err = -EIO;
	}

	if (err)
		return err;

	pr_devel("%s qid=%d tag=%d state=%d cmd-done op=%d unique=%llu\n",
		 __func__, queue->qid, ring_ent->tag, ring_ent->state,
		 req->in.h.opcode, req->in.h.unique);

	/* copy the request */
	err = fuse_uring_copy_to_ring(ring, req, ring_ent);
	if (unlikely(err)) {
		pr_info("Copy to ring failed: %d\n", err);
		goto err;
	}

	/* copy fuse_in_header */
	res = copy_to_user(&rreq->in, &req->in.h, sizeof(rreq->in));
	err = res > 0 ? -EFAULT : res;
	if (err)
		goto err;

	set_bit(FR_SENT, &req->flags);
	return 0;

err:
	fuse_uring_req_end(ring_ent, true, err);
	return err;
}

/*
 * Write data to the ring buffer and send the request to userspace,
 * userspace will read it
 * This is comparable with classical read(/dev/fuse)
 */
static int fuse_uring_send_next_to_ring(struct fuse_ring_ent *ring_ent)
{
	int err = 0;

	err = fuse_uring_prepare_send(ring_ent);
	if (err)
		goto err;

	io_uring_cmd_complete_in_task(ring_ent->cmd,
				      fuse_uring_async_send_to_ring);
	return 0;

err:
	return err;
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
 * Assign a fuse queue entry to the given entry
 */
static void fuse_uring_add_req_to_ring_ent(struct fuse_ring_ent *ring_ent,
					   struct fuse_req *req)
{
	lockdep_assert_held(&ring_ent->queue->lock);

	if (WARN_ON_ONCE(ring_ent->state != FRRS_WAIT &&
			 ring_ent->state != FRRS_COMMIT)) {
		pr_warn("%s qid=%d tag=%d state=%d async=%d\n", __func__,
			ring_ent->queue->qid, ring_ent->tag, ring_ent->state,
			ring_ent->async);
	}
	list_del_init(&req->list);
	clear_bit(FR_PENDING, &req->flags);
	ring_ent->fuse_req = req;
	ring_ent->state = FRRS_FUSE_REQ;
}

/*
 * Release the ring entry and fetch the next fuse request if available
 *
 * @return true if a new request has been fetched
 */
static bool fuse_uring_ent_assign_req(struct fuse_ring_ent *ring_ent)
	__must_hold(&queue->lock)
{
	struct fuse_req *req = NULL;
	struct fuse_ring_queue *queue = ring_ent->queue;
	struct list_head *req_queue = ring_ent->async ?
					      &queue->async_fuse_req_queue :
					      &queue->sync_fuse_req_queue;

	lockdep_assert_held(&queue->lock);

	/* get and assign the next entry while it is still holding the lock */
	if (!list_empty(req_queue)) {
		req = list_first_entry(req_queue, struct fuse_req, list);
		fuse_uring_add_req_to_ring_ent(ring_ent, req);
		list_del_init(&ring_ent->list);
	}

	return req ? true : false;
}

/*
 * Read data from the ring buffer, which user space has written to
 * This is comparible with handling of classical write(/dev/fuse).
 * Also make the ring request available again for new fuse requests.
 */
static void fuse_uring_commit(struct fuse_ring_ent *ring_ent,
			      unsigned int issue_flags)
{
	struct fuse_ring *ring = ring_ent->queue->ring;
	struct fuse_conn *fc = ring->fc;
	struct fuse_ring_req *rreq = ring_ent->rreq;
	struct fuse_req *req = ring_ent->fuse_req;
	ssize_t err = 0;
	bool set_err = false;

	err = copy_from_user(&req->out.h, &rreq->out, sizeof(req->out.h));
	if (err) {
		req->out.h.error = err;
		goto out;
	}

	err = fuse_uring_out_header_has_err(&req->out.h, req, fc);
	if (err) {
		/* req->out.h.error already set */
		pr_devel("%s:%d err=%zd oh->err=%d\n", __func__, __LINE__, err,
			 req->out.h.error);
		goto out;
	}

	err = fuse_uring_copy_from_ring(ring, req, ring_ent);
	if (err)
		set_err = true;

out:
	pr_devel("%s:%d ret=%zd op=%d req-ret=%d\n", __func__, __LINE__, err,
		 req->args->opcode, req->out.h.error);
	fuse_uring_req_end(ring_ent, set_err, err);
}

/*
 * Get the next fuse req and send it
 */
static void fuse_uring_next_fuse_req(struct fuse_ring_ent *ring_ent,
				    struct fuse_ring_queue *queue)
{
	int has_next, err;
	int prev_state = ring_ent->state;

	WARN_ON_ONCE(!list_empty(&ring_ent->list));

	do {
		spin_lock(&queue->lock);
		has_next = fuse_uring_ent_assign_req(ring_ent);
		if (!has_next) {
			fuse_uring_ent_avail(ring_ent, queue);
			spin_unlock(&queue->lock);
			break; /* no request left */
		}
		spin_unlock(&queue->lock);

		err = fuse_uring_send_next_to_ring(ring_ent);
		if (err) {
			ring_ent->state = prev_state;
			continue;
		}

		err = 0;
		spin_lock(&queue->lock);
		ring_ent->state = FRRS_USERSPACE;
		list_add(&ring_ent->list, &queue->ent_in_userspace);
		spin_unlock(&queue->lock);
	} while (err);
}

/* FUSE_URING_REQ_COMMIT_AND_FETCH handler */
static int fuse_uring_commit_fetch(struct fuse_ring_ent *ring_ent,
				   struct io_uring_cmd *cmd, int issue_flags)
__releases(ring_ent->queue->lock)
{
	int err;
	struct fuse_ring_queue *queue = ring_ent->queue;
	struct fuse_ring *ring = queue->ring;

	err = -ENOTCONN;
	if (unlikely(!ring->ready)) {
		pr_info("commit and fetch, but fuse-uring is not ready.");
		return err;
	}

	err = -EALREADY;
	if (ring_ent->state != FRRS_USERSPACE) {
		pr_info("qid=%d tag=%d state %d SQE already handled\n",
			queue->qid, ring_ent->tag, ring_ent->state);
		return err;
	}

	fuse_ring_ring_ent_unset_userspace(ring_ent);

	ring_ent->cmd = cmd;
	spin_unlock(&queue->lock);

	/* without the queue lock, as other locks are taken */
	fuse_uring_commit(ring_ent, issue_flags);

	/*
	 * Fetching the next request is absolutely required as queued
	 * fuse requests would otherwise not get processed - committing
	 * and fetching is done in one step vs legacy fuse, which has separated
	 * read (fetch request) and write (commit result).
	 */
	fuse_uring_next_fuse_req(ring_ent, queue);
	return 0;
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

/* FUSE_URING_REQ_FETCH handler */
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

	ring_ent->rreq = (void __user *)cmd_req->buf_ptr;
	ring_ent->max_arg_len = cmd_req->buf_len -
				offsetof(struct fuse_ring_req, in_out_arg);
	ret = -EINVAL;
	if (cmd_req->buf_len < ring->req_buf_sz) {
		pr_info("Invalid req buf len, expected: %zd got %d\n",
			ring->req_buf_sz, cmd_req->buf_len);
		goto err_unlock;
	}

	ring_ent->rreq = (void __user *)cmd_req->buf_ptr;
	ring_ent->max_arg_len = cmd_req->buf_len -
				offsetof(struct fuse_ring_req, in_out_arg);
	if (cmd_req->buf_len < ring->req_buf_sz) {
		pr_info("Invalid req buf len, expected: %zd got %d\n",
			ring->req_buf_sz, cmd_req->buf_len);
		goto err_unlock;
	}

	switch (cmd_op) {
	case FUSE_URING_REQ_FETCH:
		ret = fuse_uring_fetch(ring_ent, cmd, issue_flags);
		break;
	case FUSE_URING_REQ_COMMIT_AND_FETCH:
		ret = fuse_uring_commit_fetch(ring_ent, cmd, issue_flags);
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
