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

#define FUSE_RING_HEADER_PG 0
#define FUSE_RING_PAYLOAD_PG 1

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

/* Abort all list queued request on the given ring queue */
static void fuse_uring_abort_end_queue_requests(struct fuse_ring_queue *queue)
{
	struct fuse_req *req;
	LIST_HEAD(sync_list);
	LIST_HEAD(async_list);

	spin_lock(&queue->lock);

	list_for_each_entry(req, &queue->sync_fuse_req_queue, list)
		clear_bit(FR_PENDING, &req->flags);
	list_for_each_entry(req, &queue->async_fuse_req_queue, list)
		clear_bit(FR_PENDING, &req->flags);

	list_splice_init(&queue->async_fuse_req_queue, &sync_list);
	list_splice_init(&queue->sync_fuse_req_queue, &async_list);

	spin_unlock(&queue->lock);

	/* must not hold queue lock to avoid order issues with fi->lock */
	fuse_dev_end_requests(&sync_list);
	fuse_dev_end_requests(&async_list);
}

void fuse_uring_abort_end_requests(struct fuse_ring *ring)
{
	int qid;

	for (qid = 0; qid < ring->nr_queues; qid++) {
		struct fuse_ring_queue *queue = fuse_uring_get_queue(ring, qid);

		fuse_uring_abort_end_queue_requests(queue);
	}
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

	init_waitqueue_head(&ring->stop_waitq);

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

static void fuse_uring_stop_fuse_req_end(struct fuse_ring_ent *ent)
{
	struct fuse_req *req = ent->fuse_req;

	ent->fuse_req = NULL;
	clear_bit(FR_SENT, &req->flags);
	req->out.h.error = -ECONNABORTED;
	fuse_request_end(req);
}

/*
 * Copy from memmap.c, should be exported
 */
static void io_pages_free(struct page ***pages, int npages)
{
	struct page **page_array = *pages;

	if (!page_array)
		return;

	unpin_user_pages(page_array, npages);
	kvfree(page_array);
	*pages = NULL;
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
		pr_devel("qid=%d tag=%d sending cmd_done\n", queue->qid,
			 ent->tag);

		io_uring_cmd_done(ent->cmd, -ENOTCONN, 0,
				  IO_URING_F_UNLOCKED);
	}

	if (ent->fuse_req)
		fuse_uring_stop_fuse_req_end(ent);

	io_pages_free(&ent->user_pages, ent->nr_user_pages);

	ent->state = FRRS_FREED;
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
			pr_warn("entry teardown qid=%d tag=%d state=%d expected=%d",
				queue->qid, ent->tag, ent->state, exp_state);
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
	fuse_uring_stop_list_entries(&queue->async_ent_avail_queue, queue,
				     FRRS_WAIT);
	fuse_uring_stop_list_entries(&queue->sync_ent_avail_queue, queue,
				     FRRS_WAIT);
}

/*
 * Log state debug info
 */
static void fuse_uring_log_ent_state(struct fuse_ring *ring)
{
	int qid, tag;

	for (qid = 0; qid < ring->nr_queues; qid++) {
		struct fuse_ring_queue *queue = fuse_uring_get_queue(ring, qid);

		for (tag = 0; tag < ring->queue_depth; tag++) {
			struct fuse_ring_ent *ent = &queue->ring_ent[tag];

			if (ent->state != FRRS_FREED && ent->state != FRRS_INIT)
				pr_info("ring=%p qid=%d tag=%d state=%d\n",
					ring, qid, tag, ent->state);
		}
	}
	ring->stop_debug_log = 1;
}

static void fuse_uring_async_stop_queues(struct work_struct *work)
{
	int qid;
	struct fuse_ring *ring =
		container_of(work, struct fuse_ring, async_teardown_work.work);

	for (qid = 0; qid < ring->nr_queues; qid++) {
		struct fuse_ring_queue *queue = fuse_uring_get_queue(ring, qid);

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
		struct fuse_ring_queue *queue = fuse_uring_get_queue(ring, qid);

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

	/* FIXME copied from dev.c, check what 512 means  */
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
				     struct fuse_ring_ent *ent,
				     struct fuse_ring_req *rreq)
{
	struct fuse_copy_state cs;
	struct fuse_args *args = req->args;
	struct iov_iter iter;
	int res_arg_len, err;

	res_arg_len = rreq->in_out_arg_len;

	fuse_copy_init(&cs, 0, &iter);
	cs.is_uring = 1;
	cs.ring.pages = &ent->user_pages[FUSE_RING_PAYLOAD_PG];
	cs.req = req;

	err = fuse_copy_out_args(&cs, args, res_arg_len);

	return err;
}

/*
 * Copy data from the req to the ring buffer
 */
static int fuse_uring_copy_to_ring(struct fuse_ring *ring, struct fuse_req *req,
				   struct fuse_ring_ent *ent,
				   struct fuse_ring_req *rreq)
{
	struct fuse_copy_state cs;
	struct fuse_args *args = req->args;
	int err;
	struct iov_iter iter;

	fuse_copy_init(&cs, 1, &iter);
	cs.is_uring = 1;
	cs.ring.pages = &ent->user_pages[FUSE_RING_PAYLOAD_PG];
	cs.req = req;
	err = fuse_copy_args(&cs, args->in_numargs, args->in_pages,
			     (struct fuse_arg *)args->in_args, 0);
	if (err) {
		pr_info("%s fuse_copy_args failed\n", __func__);
		return err;
	}

	rreq->in_out_arg_len = cs.ring.offset;

	return err;
}

static int
fuse_uring_prepare_send(struct fuse_ring_ent *ring_ent)
{
	struct fuse_ring_req *rreq = NULL;
	struct fuse_ring_queue *queue = ring_ent->queue;
	struct fuse_ring *ring = queue->ring;
	struct fuse_req *req = ring_ent->fuse_req;
	int err = 0;

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

	rreq = kmap_local_page(ring_ent->user_pages[FUSE_RING_HEADER_PG]);

	/* copy the request */
	err = fuse_uring_copy_to_ring(ring, req, ring_ent, rreq);
	if (unlikely(err)) {
		pr_info("Copy to ring failed: %d\n", err);
		goto err;
	}

	/* copy fuse_in_header */
	rreq->in = req->in.h;

	err = 0;
	set_bit(FR_SENT, &req->flags);
out:
	if (rreq)
		kunmap_local(rreq);
	return err;
err:
	fuse_uring_req_end(ring_ent, true, err);
	goto out;
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
	struct fuse_ring_req *rreq;
	struct fuse_req *req = ring_ent->fuse_req;
	ssize_t err = 0;
	bool set_err = false;

	rreq = kmap_local_page(ring_ent->user_pages[FUSE_RING_HEADER_PG]);
	req->out.h = rreq->out;

	err = fuse_uring_out_header_has_err(&req->out.h, req, fc);
	if (err) {
		/* req->out.h.error already set */
		pr_devel("%s:%d err=%zd oh->err=%d\n", __func__, __LINE__, err,
			 req->out.h.error);
		goto out;
	}

	err = fuse_uring_copy_from_ring(ring, req, ring_ent, rreq);
	kunmap_local(rreq);
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

/*
 * Copy from memmap.c, should be exported there
 */
static struct page **io_pin_pages(unsigned long uaddr, unsigned long len,
				  int *npages)
{
	unsigned long start, end, nr_pages;
	struct page **pages;
	int ret;

	end = (uaddr + len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	start = uaddr >> PAGE_SHIFT;
	nr_pages = end - start;
	if (WARN_ON_ONCE(!nr_pages))
		return ERR_PTR(-EINVAL);

	pages = kvmalloc_array(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return ERR_PTR(-ENOMEM);

	ret = pin_user_pages_fast(uaddr, nr_pages, FOLL_WRITE | FOLL_LONGTERM,
					pages);
	/* success, mapped all pages */
	if (ret == nr_pages) {
		*npages = nr_pages;
		return pages;
	}

	/* partial map, or didn't map anything */
	if (ret >= 0) {
		/* if we did partial map, release any pages we did get */
		if (ret)
			unpin_user_pages(pages, ret);
		ret = -EFAULT;
	}
	kvfree(pages);
	return ERR_PTR(ret);
}


/* FUSE_URING_REQ_FETCH handler */
static int fuse_uring_fetch(struct fuse_ring_ent *ring_ent,
			    struct io_uring_cmd *cmd, unsigned int issue_flags)
	__releases(ring_ent->queue->lock)
{
	struct fuse_ring *ring = ring_ent->queue->ring;
	struct fuse_ring_queue *queue = ring_ent->queue;
	int err;

	/* No other bit must be set here */
	err = -EINVAL;
	if (ring_ent->state != FRRS_INIT)
		goto err_unlock;

	/*
	 * FUSE_URING_REQ_FETCH is an initialization exception, needs
	 * state override
	 */
	ring_ent->state = FRRS_USERSPACE;
	fuse_ring_ring_ent_unset_userspace(ring_ent);

	err = _fuse_uring_fetch(ring_ent, cmd, issue_flags);
	if (err)
		goto err_unlock;

	spin_unlock(&queue->lock);

	/* must not hold the queue->lock */
	ring_ent->user_pages = io_pin_pages(ring_ent->user_buf,
					    ring_ent->user_buf_len,
					    &ring_ent->nr_user_pages);
	if (IS_ERR(ring_ent->user_pages)) {
		err = PTR_ERR(ring_ent->user_pages);
		pr_info("qid=%d ent=%d pin-res=%d\n",
			queue->qid, ring_ent->tag, err);
		goto err;
	}

	/*
	 * The ring entry is registered now and needs to be handled
	 * for shutdown.
	 */
	atomic_inc(&ring->queue_refs);
	return 0;

err_unlock:
	spin_unlock(&queue->lock);
err:
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

	ring_ent->user_buf = cmd_req->buf_ptr;
	ring_ent->user_buf_len = cmd_req->buf_len;

	ring_ent->max_arg_len = cmd_req->buf_len -
				offsetof(struct fuse_ring_req, in_out_arg);
	ret = -EINVAL;
	if (cmd_req->buf_len < ring->req_buf_sz) {
		pr_info("Invalid req buf len, expected: %zd got %d\n",
			ring->req_buf_sz, cmd_req->buf_len);
		goto err_unlock;
	}

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

	if (unlikely(issue_flags & IO_URING_F_TASK_DEAD))
		goto terminating;

	err = fuse_uring_prepare_send(ring_ent);
	if (err)
		goto err;

	io_uring_cmd_done(cmd, 0, 0, issue_flags);

	spin_lock(&queue->lock);
	ring_ent->state = FRRS_USERSPACE;
	list_add(&ring_ent->list, &queue->ent_in_userspace);
	spin_unlock(&queue->lock);

	return;
err:
	fuse_uring_next_fuse_req(ring_ent, queue);

terminating:
	/* Avoid all actions as the task that issues the ring is terminating */
	io_uring_cmd_done(cmd, -ECANCELED, 0, issue_flags);
}

/* queue a fuse request and send it if a ring entry is available */
int fuse_uring_queue_fuse_req(struct fuse_conn *fc, struct fuse_req *req)
{
	struct fuse_ring *ring = fc->ring;
	struct fuse_ring_queue *queue;
	int qid = 0;
	struct fuse_ring_ent *ring_ent = NULL;
	int res;
	bool async = test_bit(FR_BACKGROUND, &req->flags);
	struct list_head *req_queue, *ent_queue;

	if (ring->per_core_queue) {
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
			      "Core number (%u) exceeds nr ueues (%zu)\n",
			      qid, ring->nr_queues))
			qid = 0;
	}

	queue = fuse_uring_get_queue(ring, qid);
	req_queue = async ? &queue->async_fuse_req_queue :
			    &queue->sync_fuse_req_queue;
	ent_queue = async ? &queue->async_ent_avail_queue :
			    &queue->sync_ent_avail_queue;

	spin_lock(&queue->lock);

	if (unlikely(queue->stopped)) {
		res = -ENOTCONN;
		goto err_unlock;
	}

	list_add_tail(&req->list, req_queue);

	if (!list_empty(ent_queue)) {
		ring_ent =
			list_first_entry(ent_queue, struct fuse_ring_ent, list);
		list_del_init(&ring_ent->list);
		fuse_uring_add_req_to_ring_ent(ring_ent, req);
	}
	spin_unlock(&queue->lock);

	if (ring_ent != NULL) {
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
