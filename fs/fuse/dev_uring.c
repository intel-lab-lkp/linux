// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE: Filesystem in Userspace
 * Copyright (c) 2023-2024 DataDirect Networks.
 */

#include <linux/fs.h>

#include "fuse_i.h"
#include "dev_uring_i.h"
#include "fuse_dev_i.h"

#include <linux/io_uring/cmd.h>

#ifdef CONFIG_FUSE_IO_URING
static bool __read_mostly enable_uring;
module_param(enable_uring, bool, 0644);
MODULE_PARM_DESC(enable_uring,
		 "Enable uring userspace communication through uring.");
#endif

struct fuse_uring_cmd_pdu {
	struct fuse_ring_ent *ring_ent;
	struct fuse_ring_queue *queue;
};

/*
 * Finalize a fuse request, then fetch and send the next entry, if available
 */
static void fuse_uring_req_end(struct fuse_ring_ent *ring_ent, bool set_err,
			       int error)
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
	struct fuse_ring_queue *queue = ent->queue;

	lockdep_assert_held(&queue->lock);

	if (WARN_ON_ONCE(ent->state != FRRS_USERSPACE))
		return -EIO;

	ent->state = FRRS_COMMIT;
	list_move(&ent->list, &queue->ent_intermediate_queue);

	return 0;
}

/* Abort all list queued request on the given ring queue */
static void fuse_uring_abort_end_queue_requests(struct fuse_ring_queue *queue)
{
	struct fuse_req *req;
	LIST_HEAD(req_list);

	spin_lock(&queue->lock);
	list_for_each_entry(req, &queue->fuse_req_queue, list)
		clear_bit(FR_PENDING, &req->flags);
	list_splice_init(&queue->fuse_req_queue, &req_list);
	spin_unlock(&queue->lock);

	/* must not hold queue lock to avoid order issues with fi->lock */
	fuse_dev_end_requests(&req_list);
}

void fuse_uring_abort_end_requests(struct fuse_ring *ring)
{
	int qid;

	for (qid = 0; qid < ring->nr_queues; qid++) {
		struct fuse_ring_queue *queue = ring->queues[qid];

		if (!queue)
			continue;

		fuse_uring_abort_end_queue_requests(queue);
	}
}

void fuse_uring_destruct(struct fuse_conn *fc)
{
	struct fuse_ring *ring = fc->ring;
	int qid;

	if (!ring)
		return;

	for (qid = 0; qid < ring->nr_queues; qid++) {
		struct fuse_ring_queue *queue = ring->queues[qid];

		if (!queue)
			continue;

		WARN_ON(!list_empty(&queue->ent_avail_queue));
		WARN_ON(!list_empty(&queue->ent_intermediate_queue));
		WARN_ON(!list_empty(&queue->ent_in_userspace));

		kfree(queue->fpq.processing);
		kfree(queue);
		ring->queues[qid] = NULL;
	}

	kfree(ring->queues);
	kfree(ring);
	fc->ring = NULL;
}

/*
 * Basic ring setup for this connection based on the provided configuration
 */
static struct fuse_ring *fuse_uring_create(struct fuse_conn *fc)
{
	struct fuse_ring *ring = NULL;
	size_t nr_queues = num_possible_cpus();
	struct fuse_ring *res = NULL;

	ring = kzalloc(sizeof(*fc->ring) +
			       nr_queues * sizeof(struct fuse_ring_queue),
		       GFP_KERNEL_ACCOUNT);
	if (!ring)
		return NULL;

	ring->queues = kcalloc(nr_queues, sizeof(struct fuse_ring_queue *),
			       GFP_KERNEL_ACCOUNT);
	if (!ring->queues)
		goto out_err;

	spin_lock(&fc->lock);
	if (fc->ring) {
		/* race, another thread created the ring in the mean time */
		spin_unlock(&fc->lock);
		res = fc->ring;
		goto out_err;
	}

	init_waitqueue_head(&ring->stop_waitq);

	fc->ring = ring;
	ring->nr_queues = nr_queues;
	ring->fc = fc;
	atomic_set(&ring->queue_refs, 0);

	spin_unlock(&fc->lock);
	return ring;

out_err:
	if (ring)
		kfree(ring->queues);
	kfree(ring);
	return res;
}

static struct fuse_ring_queue *fuse_uring_create_queue(struct fuse_ring *ring,
						       int qid)
{
	struct fuse_conn *fc = ring->fc;
	struct fuse_ring_queue *queue;
	struct list_head *pq;

	queue = kzalloc(sizeof(*queue), GFP_KERNEL_ACCOUNT);
	if (!queue)
		return ERR_PTR(-ENOMEM);
	spin_lock(&fc->lock);
	if (ring->queues[qid]) {
		spin_unlock(&fc->lock);
		kfree(queue->fpq.processing);
		kfree(queue);
		return ring->queues[qid];
	}
	ring->queues[qid] = queue;

	queue->qid = qid;
	queue->ring = ring;
	spin_lock_init(&queue->lock);

	INIT_LIST_HEAD(&queue->ent_avail_queue);
	INIT_LIST_HEAD(&queue->ent_intermediate_queue);
	INIT_LIST_HEAD(&queue->ent_in_userspace);
	INIT_LIST_HEAD(&queue->fuse_req_queue);

	pq = kcalloc(FUSE_PQ_HASH_SIZE, sizeof(struct list_head), GFP_KERNEL);
	if (!pq) {
		kfree(queue);
		return ERR_PTR(-ENOMEM);
	}
	queue->fpq.processing = pq;
	fuse_pqueue_init(&queue->fpq);

	spin_unlock(&fc->lock);

	return queue;
}

static void
fuse_uring_async_send_to_ring(struct io_uring_cmd *cmd,
			      unsigned int issue_flags)
{
	io_uring_cmd_done(cmd, 0, 0, issue_flags);
}

static void fuse_uring_stop_fuse_req_end(struct fuse_ring_ent *ent)
{
	struct fuse_req *req = ent->fuse_req;

	ent->fuse_req = NULL;
	clear_bit(FR_SENT, &req->flags);
	req->out.h.error = -ECONNABORTED;
	fuse_request_end(req);
}

/*
 * Release a request/entry on connection tear down
 */
static void fuse_uring_entry_teardown(struct fuse_ring_ent *ent,
					 bool need_cmd_done)
{
	struct fuse_ring_queue *queue = ent->queue;

	/*
	 * fuse_request_end() might take other locks like fi->lock and
	 * can lead to lock ordering issues
	 */
	lockdep_assert_not_held(&ent->queue->lock);

	if (need_cmd_done) {
		pr_devel("qid=%d sending cmd_done\n", queue->qid);

		io_uring_cmd_done(ent->cmd, -ENOTCONN, 0,
				  IO_URING_F_UNLOCKED);
	}

	if (ent->fuse_req)
		fuse_uring_stop_fuse_req_end(ent);

	list_del_init(&ent->list);
	kfree(ent);
}

static void fuse_uring_stop_list_entries(struct list_head *head,
					 struct fuse_ring_queue *queue,
					 enum fuse_ring_req_state exp_state)
{
	struct fuse_ring *ring = queue->ring;
	struct fuse_ring_ent *ent, *next;
	ssize_t queue_refs = SSIZE_MAX;
	LIST_HEAD(to_teardown);

	spin_lock(&queue->lock);
	list_for_each_entry_safe(ent, next, head, list) {
		if (ent->state != exp_state) {
			pr_warn("entry teardown qid=%d state=%d expected=%d",
				queue->qid, ent->state, exp_state);
			continue;
		}

		list_move(&ent->list, &to_teardown);
	}
	spin_unlock(&queue->lock);

	/* no queue lock to avoid lock order issues */
	list_for_each_entry_safe(ent, next, &to_teardown, list) {
		bool need_cmd_done = ent->state != FRRS_USERSPACE;

		fuse_uring_entry_teardown(ent, need_cmd_done);
		queue_refs = atomic_dec_return(&ring->queue_refs);

		if (WARN_ON_ONCE(queue_refs < 0))
			pr_warn("qid=%d queue_refs=%zd", queue->qid,
				queue_refs);
	}
}

static void fuse_uring_stop_queue(struct fuse_ring_queue *queue)
{
	fuse_uring_stop_list_entries(&queue->ent_in_userspace, queue,
				     FRRS_USERSPACE);
	fuse_uring_stop_list_entries(&queue->ent_avail_queue, queue, FRRS_WAIT);
}

/*
 * Log state debug info
 */
static void fuse_uring_log_ent_state(struct fuse_ring *ring)
{
	int qid;
	struct fuse_ring_ent *ent;

	for (qid = 0; qid < ring->nr_queues; qid++) {
		struct fuse_ring_queue *queue = ring->queues[qid];

		if (!queue)
			continue;

		spin_lock(&queue->lock);
		/*
		 * Log entries from the intermediate queue, the other queues
		 * should be empty
		 */
		list_for_each_entry(ent, &queue->ent_intermediate_queue, list) {
			pr_info("ring=%p qid=%d ent=%p state=%d\n", ring, qid,
				ent, ent->state);
		}
		spin_lock(&queue->lock);
	}
	ring->stop_debug_log = 1;
}

static void fuse_uring_async_stop_queues(struct work_struct *work)
{
	int qid;
	struct fuse_ring *ring =
		container_of(work, struct fuse_ring, async_teardown_work.work);

	for (qid = 0; qid < ring->nr_queues; qid++) {
		struct fuse_ring_queue *queue = ring->queues[qid];

		if (!queue)
			continue;

		fuse_uring_stop_queue(queue);
	}

	/*
	 * Some ring entries are might be in the middle of IO operations,
	 * i.e. in process to get handled by file_operations::uring_cmd
	 * or on the way to userspace - we could handle that with conditions in
	 * run time code, but easier/cleaner to have an async tear down handler
	 * If there are still queue references left
	 */
	if (atomic_read(&ring->queue_refs) > 0) {
		if (time_after(jiffies,
			       ring->teardown_time + FUSE_URING_TEARDOWN_TIMEOUT))
			fuse_uring_log_ent_state(ring);

		schedule_delayed_work(&ring->async_teardown_work,
				      FUSE_URING_TEARDOWN_INTERVAL);
	} else {
		wake_up_all(&ring->stop_waitq);
	}
}

/*
 * Stop the ring queues
 */
void fuse_uring_stop_queues(struct fuse_ring *ring)
{
	int qid;

	for (qid = 0; qid < ring->nr_queues; qid++) {
		struct fuse_ring_queue *queue = ring->queues[qid];

		if (!queue)
			continue;

		fuse_uring_stop_queue(queue);
	}

	if (atomic_read(&ring->queue_refs) > 0) {
		pr_info("ring=%p scheduling async queue stop\n", ring);
		ring->teardown_time = jiffies;
		INIT_DELAYED_WORK(&ring->async_teardown_work,
				  fuse_uring_async_stop_queues);
		schedule_delayed_work(&ring->async_teardown_work,
				      FUSE_URING_TEARDOWN_INTERVAL);
	} else {
		wake_up_all(&ring->stop_waitq);
	}
}

/*
 * Handle IO_URING_F_CANCEL, typically should come on on daemon termination
 */
static void fuse_uring_cancel(struct io_uring_cmd *cmd,
			      unsigned int issue_flags, struct fuse_conn *fc)
{
	struct fuse_uring_cmd_pdu *pdu = (struct fuse_uring_cmd_pdu *)cmd->pdu;
	struct fuse_ring_queue *queue = pdu->queue;
	struct fuse_ring_ent *ent;
	bool found = false;
	bool need_cmd_done = false;

	spin_lock(&queue->lock);

	/* XXX: This is cumbersome for large queues. */
	list_for_each_entry(ent, &queue->ent_avail_queue, list) {
		if (pdu->ring_ent == ent) {
			found = true;
			break;
		}
	}

	if (!found) {
		pr_info("qid=%d Did not find ent=%p", queue->qid, ent);
		spin_unlock(&queue->lock);
		return;
	}

	if (ent->state == FRRS_WAIT) {
		ent->state = FRRS_USERSPACE;
		list_move(&ent->list, &queue->ent_in_userspace);
		need_cmd_done = true;
	}
	spin_unlock(&queue->lock);

	if (need_cmd_done)
		io_uring_cmd_done(cmd, -ENOTCONN, 0, issue_flags);

	/*
	 * releasing the last entry should trigger fuse_dev_release() if
	 * the daemon was terminated
	 */
}

static void fuse_uring_prepare_cancel(struct io_uring_cmd *cmd, int issue_flags,
				      struct fuse_ring_ent *ring_ent)
{
	struct fuse_uring_cmd_pdu *pdu = (struct fuse_uring_cmd_pdu *)cmd->pdu;

	pdu->ring_ent = ring_ent;
	pdu->queue = ring_ent->queue;

	io_uring_cmd_mark_cancelable(cmd, issue_flags);
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
		pr_err("qid=%d ring-req=%p buf_req=%p invalid state %d on send\n",
		       queue->qid, ring_ent, rreq, ring_ent->state);
		err = -EIO;
	}

	if (err)
		return err;

	pr_devel("%s qid=%d state=%d cmd-done op=%d unique=%llu\n", __func__,
		 queue->qid, ring_ent->state, req->in.h.opcode,
		 req->in.h.unique);

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
	struct fuse_ring_queue *queue = ring_ent->queue;

	err = fuse_uring_prepare_send(ring_ent);
	if (err)
		goto err;

	spin_lock(&queue->lock);
	ring_ent->state = FRRS_USERSPACE;
	list_move(&ring_ent->list, &queue->ent_in_userspace);
	spin_unlock(&queue->lock);

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
	pr_devel("%s ring=%p qid=%d state=%d\n", __func__, ring,
		 ring_ent->queue->qid, ring_ent->state);

	if (WARN_ON(ring_ent->state != FRRS_COMMIT)) {
		pr_warn("%s qid=%d state=%d\n", __func__, ring_ent->queue->qid,
			ring_ent->state);
		return;
	}

	list_move(&ring_ent->list, &queue->ent_avail_queue);

	ring_ent->state = FRRS_WAIT;
}

/* Used to find the request on SQE commit */
static void fuse_uring_add_to_pq(struct fuse_ring_ent *ring_ent)
{
	struct fuse_ring_queue *queue = ring_ent->queue;
	struct fuse_req *req = ring_ent->fuse_req;
	struct fuse_pqueue *fpq = &queue->fpq;
	unsigned int hash;

	hash = fuse_req_hash(req->in.h.unique);
	list_move_tail(&req->list, &fpq->processing[hash]);
	req->ring_entry = ring_ent;
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
		pr_warn("%s qid=%d state=%d\n", __func__, ring_ent->queue->qid,
			ring_ent->state);
	}
	list_del_init(&req->list);
	clear_bit(FR_PENDING, &req->flags);
	ring_ent->fuse_req = req;
	ring_ent->state = FRRS_FUSE_REQ;

	fuse_uring_add_to_pq(ring_ent);
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
	struct list_head *req_queue = &queue->fuse_req_queue;

	lockdep_assert_held(&queue->lock);

	/* get and assign the next entry while it is still holding the lock */
	if (!list_empty(req_queue)) {
		req = list_first_entry(req_queue, struct fuse_req, list);
		fuse_uring_add_req_to_ring_ent(ring_ent, req);
		list_move(&ring_ent->list, &queue->ent_intermediate_queue);
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
	} while (err);
}

/* FUSE_URING_REQ_COMMIT_AND_FETCH handler */
static int fuse_uring_commit_fetch(struct io_uring_cmd *cmd, int issue_flags,
				   struct fuse_conn *fc)
{
	const struct fuse_uring_cmd_req *cmd_req = io_uring_sqe_cmd(cmd->sqe);
	struct fuse_ring_ent *ring_ent;
	int err;
	struct fuse_ring *ring = fc->ring;
	struct fuse_ring_queue *queue;
	uint64_t commit_id = cmd_req->commit_id;
	struct fuse_pqueue fpq;
	unsigned int hash;
	struct fuse_req *req;

	err = -ENOTCONN;
	if (!ring)
		return err;

	queue = ring->queues[cmd_req->qid];
	if (!queue)
		return err;
	fpq = queue->fpq;

	if (!READ_ONCE(fc->connected) || READ_ONCE(queue->stopped))
		return err;

	spin_lock(&queue->lock);
	/* Find a request based on the unique ID of the fuse request
	 * This should get revised, as it needs a hash calculation and list
	 * search. And full struct fuse_pqueue is needed (memory overhead).
	 * As well as the link from req to ring_ent.
	 */
	hash = fuse_req_hash(commit_id);
	req = fuse_request_find(&fpq, commit_id);
	err = -ENOENT;
	if (!req) {
		pr_info("qid=%d commit_id %llu not found\n", queue->qid,
			commit_id);
		spin_unlock(&queue->lock);
		return err;
	}
	ring_ent = req->ring_entry;
	req->ring_entry = NULL;

	err = fuse_ring_ring_ent_unset_userspace(ring_ent);
	if (err != 0) {
		pr_info_ratelimited("qid=%d commit_id %llu state %d",
				    queue->qid, commit_id, ring_ent->state);
		spin_unlock(&queue->lock);
		return err;
	}

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

static bool is_ring_ready(struct fuse_ring *ring, int current_qid)
{
	int qid;
	struct fuse_ring_queue *queue;
	bool ready = true;

	for (qid = 0; qid < ring->nr_queues && ready; qid++) {
		if (current_qid == qid)
			continue;

		queue = ring->queues[qid];
		if (!queue) {
			ready = false;
			break;
		}

		spin_lock(&queue->lock);
		if (list_empty(&queue->ent_avail_queue))
			ready = false;
		spin_unlock(&queue->lock);
	}

	return ready;
}

/*
 * fuse_uring_req_fetch command handling
 */
static void _fuse_uring_fetch(struct fuse_ring_ent *ring_ent,
			      struct io_uring_cmd *cmd,
			      unsigned int issue_flags)
{
	struct fuse_ring_queue *queue = ring_ent->queue;
	struct fuse_ring *ring = queue->ring;

	spin_lock(&queue->lock);
	fuse_uring_ent_avail(ring_ent, queue);
	ring_ent->cmd = cmd;
	spin_unlock(&queue->lock);

	if (!ring->ready) {
		bool ready = is_ring_ready(ring, queue->qid);

		if (ready)
			WRITE_ONCE(ring->ready, true);
	}
}

static int fuse_uring_fetch(struct io_uring_cmd *cmd, unsigned int issue_flags,
			    struct fuse_conn *fc)
{
	const struct fuse_uring_cmd_req *cmd_req = io_uring_sqe_cmd(cmd->sqe);
	struct fuse_ring *ring = fc->ring;
	struct fuse_ring_queue *queue;
	struct fuse_ring_ent *ring_ent;
	int err;

#if 0
	/* Does not work as sending over io-uring is async */
	err = -ETXTBSY;
	if (fc->initialized) {
		pr_info_ratelimited(
			"Received FUSE_URING_REQ_FETCH after connection is initialized\n");
		return err;
	}
#endif

	err = -ENOMEM;
	if (!ring) {
		ring = fuse_uring_create(fc);
		if (!ring)
			return err;
	}

	queue = ring->queues[cmd_req->qid];
	if (!queue) {
		queue = fuse_uring_create_queue(ring, cmd_req->qid);
		if (!queue)
			return err;
	}

	/*
	 * The created queue above does not need to be destructed in
	 * case of entry errors below, will be done at ring destruction time.
	 */

	ring_ent = kzalloc(sizeof(*ring_ent), GFP_KERNEL_ACCOUNT);
	if (ring_ent == NULL)
		return err;

	ring_ent->queue = queue;
	ring_ent->cmd = cmd;
	ring_ent->rreq = (struct fuse_ring_req __user *)cmd_req->buf_ptr;
	ring_ent->max_arg_len = cmd_req->buf_len - sizeof(*ring_ent->rreq);
	INIT_LIST_HEAD(&ring_ent->list);

	spin_lock(&queue->lock);

	/*
	 * FUSE_URING_REQ_FETCH is an initialization exception, needs
	 * state override
	 */
	ring_ent->state = FRRS_USERSPACE;
	err = fuse_ring_ring_ent_unset_userspace(ring_ent);
	spin_unlock(&queue->lock);
	if (WARN_ON_ONCE(err != 0))
		goto err;

	atomic_inc(&ring->queue_refs);
	fuse_uring_prepare_cancel(cmd, issue_flags, ring_ent);
	_fuse_uring_fetch(ring_ent, cmd, issue_flags);

	return 0;
err:
	list_del_init(&ring_ent->list);
	kfree(ring_ent);
	return err;
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
	u32 cmd_op = cmd->cmd_op;
	int err = 0;

	pr_devel("%s:%d received: cmd op %d\n", __func__, __LINE__, cmd_op);

	err = -EOPNOTSUPP;
	if (!enable_uring) {
		pr_info_ratelimited("uring is disabled\n");
		goto out;
	}

	err = -ENOTCONN;
	fud = fuse_get_dev(cmd->file);
	if (!fud) {
		pr_info_ratelimited("No fuse device found\n");
		goto out;
	}
	fc = fud->fc;

	if (fc->aborted)
		goto out;

	if ((unlikely(issue_flags & IO_URING_F_CANCEL))) {
		fuse_uring_cancel(cmd, issue_flags, fc);
		return 0;
	}

	switch (cmd_op) {
	case FUSE_URING_REQ_FETCH:
		err = fuse_uring_fetch(cmd, issue_flags, fc);
		break;
	case FUSE_URING_REQ_COMMIT_AND_FETCH:
		err = fuse_uring_commit_fetch(cmd, issue_flags, fc);
		break;
	default:
		err = -EINVAL;
		pr_devel("Unknown uring command %d", cmd_op);
		goto out;
	}
out:
	pr_devel("uring cmd op=%d, qid=%d ID=%llu ret=%d\n", cmd_op,
		 cmd_req->qid, cmd_req->commit_id, err);

	if (err < 0)
		io_uring_cmd_done(cmd, err, 0, issue_flags);

	return -EIOCBQUEUED;
}

/*
 * This prepares and sends the ring request in fuse-uring task context.
 * User buffers are not mapped yet - the application does not have permission
 * to write to it - this has to be executed in ring task context.
 * XXX: Map and pin user paged and avoid this function.
 */
static void
fuse_uring_send_req_in_task(struct io_uring_cmd *cmd,
			    unsigned int issue_flags)
{
	struct fuse_uring_cmd_pdu *pdu = (struct fuse_uring_cmd_pdu *)cmd->pdu;
	struct fuse_ring_ent *ring_ent = pdu->ring_ent;
	struct fuse_ring_queue *queue = ring_ent->queue;
	int err;

	BUILD_BUG_ON(sizeof(pdu) > sizeof(cmd->pdu));

	if (unlikely(issue_flags & IO_URING_F_TASK_DEAD)) {
		err = -ECANCELED;
		goto terminating;
	}

	err = fuse_uring_prepare_send(ring_ent);
	if (err)
		goto err;

terminating:
	spin_lock(&queue->lock);
	ring_ent->state = FRRS_USERSPACE;
	list_move(&ring_ent->list, &queue->ent_in_userspace);
	spin_unlock(&queue->lock);
	io_uring_cmd_done(cmd, err, 0, issue_flags);

	return;
err:
	fuse_uring_next_fuse_req(ring_ent, queue);
}

/* queue a fuse request and send it if a ring entry is available */
int fuse_uring_queue_fuse_req(struct fuse_conn *fc, struct fuse_req *req)
{
	struct fuse_ring *ring = fc->ring;
	struct fuse_ring_queue *queue;
	int qid = 0;
	struct fuse_ring_ent *ring_ent = NULL;
	int res;

	/*
	 * async requests are best handled on another core, the current
	 * core can do application/page handling, while the async request
	 * is handled on another core in userspace.
	 * For sync request the application has to wait - no processing, so
	 * the request should continue on the current core and avoid context
	 * switches.
	 * XXX This should be on the same numa node and not busy - is there
	 * a scheduler function available  that could make this decision?
	 * It should also not persistently switch between cores - makes
	 * it hard for the scheduler.
	 */
	qid = task_cpu(current);

	if (WARN_ONCE(qid >= ring->nr_queues,
		      "Core number (%u) exceeds nr ueues (%zu)\n", qid,
		      ring->nr_queues))
		qid = 0;

	queue = ring->queues[qid];
	if (WARN_ONCE(!queue, "Missing queue for qid %d\n", qid))
		return -EINVAL;

	spin_lock(&queue->lock);

	if (unlikely(queue->stopped)) {
		res = -ENOTCONN;
		goto err_unlock;
	}

	list_add_tail(&req->list, &queue->fuse_req_queue);

	if (!list_empty(&queue->ent_avail_queue)) {
		ring_ent = list_first_entry(&queue->ent_avail_queue,
					    struct fuse_ring_ent, list);
		list_del_init(&ring_ent->list);
		fuse_uring_add_req_to_ring_ent(ring_ent, req);
	}
	spin_unlock(&queue->lock);

	if (ring_ent) {
		struct io_uring_cmd *cmd = ring_ent->cmd;
		struct fuse_uring_cmd_pdu *pdu =
			(struct fuse_uring_cmd_pdu *)cmd->pdu;

		pdu->ring_ent = ring_ent;
		io_uring_cmd_complete_in_task(cmd, fuse_uring_send_req_in_task);
	}

	return 0;

err_unlock:
	spin_unlock(&queue->lock);
	return res;
}
