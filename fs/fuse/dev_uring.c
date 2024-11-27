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

bool fuse_uring_enabled(void)
{
	return enable_uring;
}

struct fuse_uring_cmd_pdu {
	struct fuse_ring_ent *ring_ent;
	struct fuse_ring_queue *queue;
};

const struct fuse_iqueue_ops fuse_io_uring_ops;

static void fuse_uring_flush_bg(struct fuse_ring_queue *queue)
{
	struct fuse_ring *ring = queue->ring;
	struct fuse_conn *fc = ring->fc;

	lockdep_assert_held(&queue->lock);
	lockdep_assert_held(&fc->bg_lock);

	/*
	 * Allow one bg request per queue, ignoring global fc limits.
	 * This prevents a single queue from consuming all resources and
	 * eliminates the need for remote queue wake-ups when global
	 * limits are met but this queue has no more waiting requests.
	 */
	while ((fc->active_background < fc->max_background ||
		!queue->active_background) &&
	       (!list_empty(&queue->fuse_req_bg_queue))) {
		struct fuse_req *req;

		req = list_first_entry(&queue->fuse_req_bg_queue,
				       struct fuse_req, list);
		fc->active_background++;
		queue->active_background++;

		list_move_tail(&req->list, &queue->fuse_req_queue);
	}
}

static void fuse_uring_req_end(struct fuse_ring_ent *ring_ent, bool set_err,
			       int error)
{
	struct fuse_ring_queue *queue = ring_ent->queue;
	struct fuse_req *req = ring_ent->fuse_req;
	struct fuse_ring *ring = queue->ring;
	struct fuse_conn *fc = ring->fc;

	lockdep_assert_not_held(&queue->lock);
	spin_lock(&queue->lock);
	if (test_bit(FR_BACKGROUND, &req->flags)) {
		queue->active_background--;
		spin_lock(&fc->bg_lock);
		fuse_uring_flush_bg(queue);
		spin_unlock(&fc->bg_lock);
	}

	spin_unlock(&queue->lock);

	if (set_err)
		req->out.h.error = error;

	clear_bit(FR_SENT, &req->flags);
	fuse_request_end(ring_ent->fuse_req);
	ring_ent->fuse_req = NULL;
}

static int fuse_ring_ent_unset_userspace(struct fuse_ring_ent *ent)
{
	struct fuse_ring_queue *queue = ent->queue;

	lockdep_assert_held(&queue->lock);

	if (WARN_ON_ONCE(ent->state != FRRS_USERSPACE))
		return -EIO;

	ent->state = FRRS_COMMIT;
	list_move(&ent->list, &queue->ent_commit_queue);

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
	struct fuse_ring_queue *queue;
	struct fuse_conn *fc = ring->fc;

	for (qid = 0; qid < ring->nr_queues; qid++) {
		queue = READ_ONCE(ring->queues[qid]);
		if (!queue)
			continue;

		queue->stopped = true;

		WARN_ON_ONCE(ring->fc->max_background != UINT_MAX);
		spin_lock(&queue->lock);
		spin_lock(&fc->bg_lock);
		fuse_uring_flush_bg(queue);
		spin_unlock(&fc->bg_lock);
		spin_unlock(&queue->lock);
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
		WARN_ON(!list_empty(&queue->ent_w_req_queue));
		WARN_ON(!list_empty(&queue->ent_commit_queue));
		WARN_ON(!list_empty(&queue->ent_in_userspace));

		kfree(queue->fpq.processing);
		kfree(queue);
		ring->queues[qid] = NULL;
	}

	kfree(ring->queues);
	kfree(ring);
	fc->ring = NULL;
}

#define FUSE_URING_IOV_SEGS 2 /* header and payload */

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
	struct fuse_ring_ent *ent, *next;

	queue = kzalloc(sizeof(*queue), GFP_KERNEL_ACCOUNT);
	if (!queue)
		return ERR_PTR(-ENOMEM);
	pq = kcalloc(FUSE_PQ_HASH_SIZE, sizeof(struct list_head), GFP_KERNEL);
	if (!pq) {
		kfree(queue);
		return ERR_PTR(-ENOMEM);
	}

	spin_lock(&fc->lock);
	if (ring->queues[qid]) {
		spin_unlock(&fc->lock);
		kfree(queue->fpq.processing);
		kfree(queue);
		return ring->queues[qid];
	}

	queue->qid = qid;
	queue->ring = ring;
	spin_lock_init(&queue->lock);

	INIT_LIST_HEAD(&queue->ent_avail_queue);
	INIT_LIST_HEAD(&queue->ent_commit_queue);
	INIT_LIST_HEAD(&queue->ent_w_req_queue);
	INIT_LIST_HEAD(&queue->ent_in_userspace);
	INIT_LIST_HEAD(&queue->fuse_req_queue);
	INIT_LIST_HEAD(&queue->fuse_req_bg_queue);
	INIT_LIST_HEAD(&queue->ent_released);

	list_for_each_entry_safe(ent, next, &queue->ent_released, list) {
		list_del_init(&ent->list);
		kfree(ent);
	}

	queue->fpq.processing = pq;
	fuse_pqueue_init(&queue->fpq);

	WRITE_ONCE(ring->queues[qid], queue);
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

	/* remove entry from fuse_pqueue->processing */
	list_del_init(&req->list);
	ent->fuse_req = NULL;
	clear_bit(FR_SENT, &req->flags);
	req->out.h.error = -ECONNABORTED;
	fuse_request_end(req);
}

/*
 * Release a request/entry on connection tear down
 */
static void fuse_uring_entry_teardown(struct fuse_ring_ent *ent)
{
	struct fuse_ring_queue *queue = ent->queue;

	if (ent->need_cmd_done)
		io_uring_cmd_done(ent->cmd, -ENOTCONN, 0,
				  IO_URING_F_UNLOCKED);

	if (ent->fuse_req)
		fuse_uring_stop_fuse_req_end(ent);

	/*
	 * The entry must not be freed immediately, due to access of direct
	 * pointer access of entries through IO_URING_F_CANCEL - there is a risk
	 * of race between daemon termination (which triggers IO_URING_F_CANCEL
	 * and accesses entries without checking the list state first
	 */
	spin_lock(&queue->lock);
	list_move(&ent->list, &queue->ent_released);
	ent->state = FRRS_RELEASED;
	spin_unlock(&queue->lock);
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

		ent->need_cmd_done = ent->state != FRRS_USERSPACE;
		ent->state = FRRS_TEARDOWN;
		list_move(&ent->list, &to_teardown);
	}
	spin_unlock(&queue->lock);

	/* no queue lock to avoid lock order issues */
	list_for_each_entry_safe(ent, next, &to_teardown, list) {
		fuse_uring_entry_teardown(ent);
		queue_refs = atomic_dec_return(&ring->queue_refs);

		WARN_ON_ONCE(queue_refs < 0);
	}
}

static void fuse_uring_teardown_entries(struct fuse_ring_queue *queue)
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
		list_for_each_entry(ent, &queue->ent_w_req_queue, list) {
			pr_info(" ent-req-queue ring=%p qid=%d ent=%p state=%d\n",
				ring, qid, ent, ent->state);
		}
		list_for_each_entry(ent, &queue->ent_commit_queue, list) {
			pr_info(" ent-req-queue ring=%p qid=%d ent=%p state=%d\n",
				ring, qid, ent, ent->state);
		}
		spin_unlock(&queue->lock);
	}
	ring->stop_debug_log = 1;
}

static void fuse_uring_async_stop_queues(struct work_struct *work)
{
	int qid;
	struct fuse_ring *ring =
		container_of(work, struct fuse_ring, async_teardown_work.work);

	/* XXX code dup */
	for (qid = 0; qid < ring->nr_queues; qid++) {
		struct fuse_ring_queue *queue = READ_ONCE(ring->queues[qid]);

		if (!queue)
			continue;

		fuse_uring_teardown_entries(queue);
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
		struct fuse_ring_queue *queue = READ_ONCE(ring->queues[qid]);

		if (!queue)
			continue;

		fuse_uring_teardown_entries(queue);
	}

	if (atomic_read(&ring->queue_refs) > 0) {
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
 * Handle IO_URING_F_CANCEL, typically should come on daemon termination
 */
static void fuse_uring_cancel(struct io_uring_cmd *cmd,
			      unsigned int issue_flags, struct fuse_conn *fc)
{
	struct fuse_uring_cmd_pdu *pdu = (struct fuse_uring_cmd_pdu *)cmd->pdu;
	struct fuse_ring_queue *queue = pdu->queue;
	struct fuse_ring_ent *ent = pdu->ring_ent;
	bool need_cmd_done = false;

	/*
	 * direct access on ent - it must not be destructed as long as
	 * IO_URING_F_CANCEL might come up
	 */
	spin_lock(&queue->lock);
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

	err = -EINVAL;
	if (oh->unique == 0) {
		/* Not supportd through io-uring yet */
		pr_warn_once("fuse: notify through fuse-io-uring not supported\n");
		goto seterr;
	}

	err = -EINVAL;
	if (oh->error <= -ERESTARTSYS || oh->error > 0)
		goto seterr;

	if (oh->error) {
		err = oh->error;
		goto err;
	}

	err = -ENOENT;
	if ((oh->unique & ~FUSE_INT_REQ_BIT) != req->in.h.unique) {
		pr_warn_ratelimited("Unexpected seqno mismatch, expected: %llu got %llu\n",
			req->in.h.unique, oh->unique & ~FUSE_INT_REQ_BIT);
		goto seterr;
	}

	/*
	 * Is it an interrupt reply ID?
	 * XXX: Not supported through fuse-io-uring yet, it should not even
	 *      find the request - should not happen.
	 */
	WARN_ON_ONCE(oh->unique & FUSE_INT_REQ_BIT);

	return 0;

seterr:
	oh->error = err;
err:
	return err;
}

static int fuse_uring_copy_from_ring(struct fuse_ring *ring,
				     struct fuse_req *req,
				     struct fuse_ring_ent *ent)
{
	struct fuse_copy_state cs;
	struct fuse_args *args = req->args;
	struct iov_iter iter;
	int err, res;
	struct fuse_uring_ent_in_out ring_in_out;

	res = copy_from_user(&ring_in_out, &ent->headers->ring_ent_in_out,
			     sizeof(ring_in_out));
	if (res)
		return -EFAULT;

	err = import_ubuf(ITER_SOURCE, ent->payload, ent->max_arg_len, &iter);
	if (err)
		return err;

	fuse_copy_init(&cs, 0, &iter);
	cs.is_uring = 1;
	cs.req = req;

	return fuse_copy_out_args(&cs, args, ring_in_out.payload_sz);
}

 /*
  * Copy data from the req to the ring buffer
  */
static int fuse_uring_copy_to_ring(struct fuse_ring *ring, struct fuse_req *req,
				   struct fuse_ring_ent *ent)
{
	struct fuse_copy_state cs;
	struct fuse_args *args = req->args;
	struct fuse_in_arg *in_args = args->in_args;
	int num_args = args->in_numargs;
	int err, res;
	struct iov_iter iter;
	struct fuse_uring_ent_in_out ring_in_out = { .flags = 0 };

	if (num_args == 0)
		return 0;

	err = import_ubuf(ITER_DEST, ent->payload, ent->max_arg_len, &iter);
	if (err) {
		pr_info_ratelimited("fuse: Import of user buffer failed\n");
		return err;
	}

	fuse_copy_init(&cs, 1, &iter);
	cs.is_uring = 1;
	cs.req = req;

	/*
	 * Expectation is that the first argument is the per op header.
	 * Some op code have that as zero.
	 */
	if (args->in_args[0].size > 0) {
		res = copy_to_user(&ent->headers->op_in, in_args->value,
				   in_args->size);
		err = res > 0 ? -EFAULT : res;
		if (err) {
			pr_info_ratelimited("Copying the header failed.\n");
			return err;
		}
	}
	in_args++;
	num_args--;

	/* copy the payload */
	err = fuse_copy_args(&cs, num_args, args->in_pages,
			     (struct fuse_arg *)in_args, 0);
	if (err) {
		pr_info_ratelimited("%s fuse_copy_args failed\n", __func__);
		return err;
	}

	ring_in_out.payload_sz = cs.ring.offset;
	res = copy_to_user(&ent->headers->ring_ent_in_out, &ring_in_out,
			   sizeof(ring_in_out));
	err = res > 0 ? -EFAULT : res;
	if (err)
		return err;

	return 0;
}

static int
fuse_uring_prepare_send(struct fuse_ring_ent *ring_ent)
{
	struct fuse_ring_queue *queue = ring_ent->queue;
	struct fuse_ring *ring = queue->ring;
	struct fuse_req *req = ring_ent->fuse_req;
	int err = 0, res;

	if (WARN_ON(ring_ent->state != FRRS_FUSE_REQ)) {
		pr_err("qid=%d ring-req=%p invalid state %d on send\n",
		       queue->qid, ring_ent, ring_ent->state);
		err = -EIO;
	}

	if (err)
		return err;

	/* copy the request */
	err = fuse_uring_copy_to_ring(ring, req, ring_ent);
	if (unlikely(err)) {
		pr_info("Copy to ring failed: %d\n", err);
		goto err;
	}

	/* copy fuse_in_header */
	res = copy_to_user(&ring_ent->headers->in_out, &req->in.h,
			   sizeof(req->in.h));
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
 * Make a ring entry available for fuse_req assignment
 */
static void fuse_uring_ent_avail(struct fuse_ring_ent *ring_ent,
				 struct fuse_ring_queue *queue,
				 unsigned int issue_flags)
{
	fuse_uring_prepare_cancel(ring_ent->cmd, issue_flags, ring_ent);
	list_move(&ring_ent->list, &queue->ent_avail_queue);
	ring_ent->state = FRRS_WAIT;
}

/* Used to find the request on SQE commit */
static void fuse_uring_add_to_pq(struct fuse_ring_ent *ring_ent,
				 struct fuse_req *req)
{
	struct fuse_ring_queue *queue = ring_ent->queue;
	struct fuse_pqueue *fpq = &queue->fpq;
	unsigned int hash;

	req->ring_entry = ring_ent;
	hash = fuse_req_hash(req->in.h.unique);
	list_move_tail(&req->list, &fpq->processing[hash]);
}

/*
 * Assign a fuse queue entry to the given entry
 */
static void fuse_uring_add_req_to_ring_ent(struct fuse_ring_ent *ring_ent,
					   struct fuse_req *req)
{
	struct fuse_ring_queue *queue = ring_ent->queue;

	lockdep_assert_held(&queue->lock);

	if (WARN_ON_ONCE(ring_ent->state != FRRS_WAIT &&
			 ring_ent->state != FRRS_COMMIT)) {
		pr_warn("%s qid=%d state=%d\n", __func__, ring_ent->queue->qid,
			ring_ent->state);
	}
	list_del_init(&req->list);
	clear_bit(FR_PENDING, &req->flags);
	ring_ent->fuse_req = req;
	ring_ent->state = FRRS_FUSE_REQ;
	list_move(&ring_ent->list, &queue->ent_w_req_queue);
	fuse_uring_add_to_pq(ring_ent, req);
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
	struct fuse_req *req = ring_ent->fuse_req;
	ssize_t err = 0;
	bool set_err = false;

	err = copy_from_user(&req->out.h, &ring_ent->headers->in_out,
			     sizeof(req->out.h));
	if (err) {
		req->out.h.error = err;
		goto out;
	}

	err = fuse_uring_out_header_has_err(&req->out.h, req, fc);
	if (err) {
		/* req->out.h.error already set */
		goto out;
	}

	err = fuse_uring_copy_from_ring(ring, req, ring_ent);
	if (err)
		set_err = true;

out:
	fuse_uring_req_end(ring_ent, set_err, err);
}

/*
 * Get the next fuse req and send it
 */
static void fuse_uring_next_fuse_req(struct fuse_ring_ent *ring_ent,
				    struct fuse_ring_queue *queue,
				    unsigned int issue_flags)
{
	int has_next, err;
	int prev_state = ring_ent->state;

	do {
		spin_lock(&queue->lock);
		has_next = fuse_uring_ent_assign_req(ring_ent);
		if (!has_next) {
			fuse_uring_ent_avail(ring_ent, queue, issue_flags);
			spin_unlock(&queue->lock);
			break; /* no request left */
		}
		spin_unlock(&queue->lock);

		err = fuse_uring_send_next_to_ring(ring_ent);
		if (err)
			ring_ent->state = prev_state;
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
	req = fuse_request_find(&fpq, commit_id);
	err = -ENOENT;
	if (!req) {
		pr_info("qid=%d commit_id %llu not found\n", queue->qid,
			commit_id);
		spin_unlock(&queue->lock);
		return err;
	}
	list_del_init(&req->list);
	ring_ent = req->ring_entry;
	req->ring_entry = NULL;

	err = fuse_ring_ent_unset_userspace(ring_ent);
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
	fuse_uring_next_fuse_req(ring_ent, queue, issue_flags);
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
	struct fuse_conn *fc = ring->fc;
	struct fuse_iqueue *fiq = &fc->iq;

	spin_lock(&queue->lock);
	fuse_uring_ent_avail(ring_ent, queue, issue_flags);
	ring_ent->cmd = cmd;
	spin_unlock(&queue->lock);

	if (!ring->ready) {
		bool ready = is_ring_ready(ring, queue->qid);

		if (ready) {
			WRITE_ONCE(ring->ready, true);
			fiq->ops = &fuse_io_uring_ops;
		}
	}
}

/*
 * sqe->addr is a ptr to an iovec array, iov[0] has the headers, iov[1]
 * the payload
 */
static int fuse_uring_get_iovec_from_sqe(const struct io_uring_sqe *sqe,
					 struct iovec iov[FUSE_URING_IOV_SEGS])
{
	struct iovec __user *uiov = u64_to_user_ptr(READ_ONCE(sqe->addr));
	struct iov_iter iter;
	ssize_t ret;

	if (sqe->len != FUSE_URING_IOV_SEGS)
		return -EINVAL;

	/*
	 * Direction for buffer access will actually be READ and WRITE,
	 * using write for the import should include READ access as well.
	 */
	ret = import_iovec(WRITE, uiov, FUSE_URING_IOV_SEGS,
			   FUSE_URING_IOV_SEGS, &iov, &iter);
	if (ret < 0)
		return ret;

	return 0;
}

static int fuse_uring_fetch(struct io_uring_cmd *cmd, unsigned int issue_flags,
			    struct fuse_conn *fc)
{
	const struct fuse_uring_cmd_req *cmd_req = io_uring_sqe_cmd(cmd->sqe);
	struct fuse_ring *ring = fc->ring;
	struct fuse_ring_queue *queue;
	struct fuse_ring_ent *ring_ent;
	int err;
	struct iovec iov[FUSE_URING_IOV_SEGS];

	err = fuse_uring_get_iovec_from_sqe(cmd->sqe, iov);
	if (err) {
		pr_info_ratelimited("Failed to get iovec from sqe, err=%d\n",
				    err);
		return err;
	}

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

	INIT_LIST_HEAD(&ring_ent->list);

	ring_ent->queue = queue;
	ring_ent->cmd = cmd;

	err = -EINVAL;
	if (iov[0].iov_len < sizeof(struct fuse_uring_req_header)) {
		pr_info_ratelimited("Invalid header len %zu\n", iov[0].iov_len);
		goto err;
	}

	ring_ent->headers = iov[0].iov_base;
	ring_ent->payload = iov[1].iov_base;
	ring_ent->max_arg_len = iov[1].iov_len;

	if (ring_ent->max_arg_len <
	    max_t(size_t, FUSE_MIN_READ_BUFFER, fc->max_write)) {
		pr_info_ratelimited("Invalid req payload len %zu\n",
				    ring_ent->max_arg_len);
		goto err;
	}

	spin_lock(&queue->lock);

	/*
	 * FUSE_URING_REQ_FETCH is an initialization exception, needs
	 * state override
	 */
	ring_ent->state = FRRS_USERSPACE;
	err = fuse_ring_ent_unset_userspace(ring_ent);
	spin_unlock(&queue->lock);
	if (WARN_ON_ONCE(err != 0))
		goto err;

	atomic_inc(&ring->queue_refs);
	_fuse_uring_fetch(ring_ent, cmd, issue_flags);

	return 0;
err:
	list_del_init(&ring_ent->list);
	kfree(ring_ent);
	return err;
}

/*
 * Entry function from io_uring to handle the given passthrough command
 * (op cocde IORING_OP_URING_CMD)
 */
int fuse_uring_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
	struct fuse_dev *fud;
	struct fuse_conn *fc;
	u32 cmd_op = cmd->cmd_op;
	int err;


	if (!enable_uring) {
		pr_info_ratelimited("fuse-io-uring is disabled\n");
		return -EOPNOTSUPP;
	}

	fud = fuse_get_dev(cmd->file);
	if (!fud) {
		pr_info_ratelimited("No fuse device found\n");
		return -ENOTCONN;
	}
	fc = fud->fc;

	if (!fc->connected || fc->aborted)
		return fc->aborted ? -ECONNABORTED : -ENOTCONN;

	if ((unlikely(issue_flags & IO_URING_F_CANCEL))) {
		fuse_uring_cancel(cmd, issue_flags, fc);
		return 0;
	}

	switch (cmd_op) {
	case FUSE_URING_REQ_FETCH:
		err = fuse_uring_fetch(cmd, issue_flags, fc);
		if (err) {
			pr_info_once("fuse_uring_fetch failed err=%d\n", err);
			return err;
		}
		break;
	case FUSE_URING_REQ_COMMIT_AND_FETCH:
		err = fuse_uring_commit_fetch(cmd, issue_flags, fc);
		break;
	default:
		return -EINVAL;
	}

	return -EIOCBQUEUED;
}

/*
 * This prepares and sends the ring request in fuse-uring task context.
 * User buffers are not mapped yet - the application does not have permission
 * to write to it - this has to be executed in ring task context.
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
	fuse_uring_next_fuse_req(ring_ent, queue, issue_flags);
}

static struct fuse_ring_queue *fuse_uring_task_to_queue(struct fuse_ring *ring)
{
	unsigned int qid;
	struct fuse_ring_queue *queue;

	qid = task_cpu(current);

	if (WARN_ONCE(qid >= ring->nr_queues,
		      "Core number (%u) exceeds nr ueues (%zu)\n", qid,
		      ring->nr_queues))
		qid = 0;

	queue = ring->queues[qid];
	if (WARN_ONCE(!queue, "Missing queue for qid %d\n", qid))
		return NULL;

	return queue;
}

/* queue a fuse request and send it if a ring entry is available */
void fuse_uring_queue_fuse_req(struct fuse_iqueue *fiq, struct fuse_req *req)
{
	struct fuse_conn *fc = req->fm->fc;
	struct fuse_ring *ring = fc->ring;
	struct fuse_ring_queue *queue;
	struct fuse_ring_ent *ring_ent = NULL;
	int err;

	err = -EINVAL;
	queue = fuse_uring_task_to_queue(ring);
	if (!queue)
		goto err;

	if (req->in.h.opcode != FUSE_NOTIFY_REPLY)
		req->in.h.unique = fuse_get_unique(fiq);
	spin_lock(&queue->lock);
	err = -ENOTCONN;
	if (unlikely(queue->stopped))
		goto err_unlock;

	if (!list_empty(&queue->ent_avail_queue)) {
		ring_ent = list_first_entry(&queue->ent_avail_queue,
					    struct fuse_ring_ent, list);

		fuse_uring_add_req_to_ring_ent(ring_ent, req);
	} else {
		list_add_tail(&req->list, &queue->fuse_req_queue);
	}
	spin_unlock(&queue->lock);

	if (ring_ent) {
		struct io_uring_cmd *cmd = ring_ent->cmd;
		err = -EIO;
		if (WARN_ON_ONCE(ring_ent->state != FRRS_FUSE_REQ))
			goto err;

		/* pdu already set by preparing IO_URING_F_CANCEL */
		io_uring_cmd_complete_in_task(cmd, fuse_uring_send_req_in_task);
	}

	return;

err_unlock:
	spin_unlock(&queue->lock);
err:
	req->out.h.error = err;
	clear_bit(FR_PENDING, &req->flags);
	fuse_request_end(req);
}

bool fuse_uring_queue_bq_req(struct fuse_req *req)
{
	struct fuse_conn *fc = req->fm->fc;
	struct fuse_ring *ring = fc->ring;
	struct fuse_ring_queue *queue;
	struct fuse_ring_ent *ring_ent = NULL;

	queue = fuse_uring_task_to_queue(ring);
	if (!queue)
		return false;

	spin_lock(&queue->lock);
	if (unlikely(queue->stopped)) {
		spin_unlock(&queue->lock);
		return false;
	}

	list_add_tail(&req->list, &queue->fuse_req_bg_queue);

	if (!list_empty(&queue->ent_avail_queue))
		ring_ent = list_first_entry(&queue->ent_avail_queue,
					    struct fuse_ring_ent, list);

	spin_lock(&fc->bg_lock);
	fc->num_background++;
	if (fc->num_background == fc->max_background)
		fc->blocked = 1;
	fuse_uring_flush_bg(queue);
	spin_unlock(&fc->bg_lock);

	/*
	 * Due to bg_queue flush limits there might be other bg requests
	 * in the queue that need to be handled first. Or no further req
	 * might be available.
	 */
	req = list_first_entry_or_null(&queue->fuse_req_queue, struct fuse_req,
				       list);
	if (ring_ent && req) {
		struct io_uring_cmd *cmd = ring_ent->cmd;

		fuse_uring_add_req_to_ring_ent(ring_ent, req);

		/* pdu already set by preparing IO_URING_F_CANCEL */
		io_uring_cmd_complete_in_task(cmd, fuse_uring_send_req_in_task);
	}
	spin_unlock(&queue->lock);

	return true;
}

const struct fuse_iqueue_ops fuse_io_uring_ops = {
	/* should be send over io-uring as enhancement */
	.send_forget = fuse_dev_queue_forget,

	/*
	 * could be send over io-uring, but interrupts should be rare,
	 * no need to make the code complex
	 */
	.send_interrupt = fuse_dev_queue_interrupt,
	.send_req = fuse_uring_queue_fuse_req,
};
