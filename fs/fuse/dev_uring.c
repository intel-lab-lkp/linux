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

static void fuse_uring_queue_cfg(struct fuse_ring_queue *queue, int qid,
				 struct fuse_ring *ring)
{
	int tag;

	queue->qid = qid;
	queue->ring = ring;

	for (tag = 0; tag < ring->queue_depth; tag++) {
		struct fuse_ring_ent *ent = &queue->ring_ent[tag];

		ent->queue = queue;
		ent->tag = tag;
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
