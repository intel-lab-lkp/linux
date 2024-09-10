/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RQ_QOS_H
#define RQ_QOS_H

#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/blk_types.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/blk-mq.h>

#include "blk-mq-debugfs.h"

struct blk_mq_debugfs_attr;

enum rq_qos_id {
	RQ_QOS_WBT,
	RQ_QOS_LATENCY,
	RQ_QOS_COST,
};

struct rq_wait {
	wait_queue_head_t wait;
	atomic_t inflight;
};

struct rq_qos {
	const struct rq_qos_ops *ops;
	struct gendisk *disk;
	enum rq_qos_id id;
	struct rq_qos *next;
#ifdef CONFIG_BLK_DEBUG_FS
	struct dentry *debugfs_dir;
#endif
};

struct rq_qos_ops {
	void (*throttle)(struct rq_qos *, struct bio *);
	void (*track)(struct rq_qos *, struct request *, struct bio *);
	void (*merge)(struct rq_qos *, struct request *, struct bio *);
	void (*issue)(struct rq_qos *, struct request *);
	void (*requeue)(struct rq_qos *, struct request *);
	void (*done)(struct rq_qos *, struct request *);
	void (*done_bio)(struct rq_qos *, struct bio *);
	void (*cleanup)(struct rq_qos *, struct bio *);
	void (*queue_depth_changed)(struct rq_qos *);
	void (*exit)(struct rq_qos *);
	const struct blk_mq_debugfs_attr *debugfs_attrs;
};

struct rq_depth {
	unsigned int max_depth;

	int scale_step;
	bool scaled_max;

	unsigned int queue_depth;
	unsigned int default_depth;
};

#define for_each_rqos(rqos, q)	\
	for (rqos = q->rq_qos; rqos; rqos = rqos->next)

static inline struct rq_qos *rq_qos_id(struct request_queue *q,
				       enum rq_qos_id id)
{
	struct rq_qos *rqos;
	for_each_rqos(rqos, q) {
		if (rqos->id == id)
			break;
	}
	return rqos;
}

static inline struct rq_qos *wbt_rq_qos(struct request_queue *q)
{
	return rq_qos_id(q, RQ_QOS_WBT);
}

static inline struct rq_qos *iolat_rq_qos(struct request_queue *q)
{
	return rq_qos_id(q, RQ_QOS_LATENCY);
}

static inline void rq_wait_init(struct rq_wait *rq_wait)
{
	atomic_set(&rq_wait->inflight, 0);
	init_waitqueue_head(&rq_wait->wait);
}

int rq_qos_add(struct rq_qos *rqos, struct gendisk *disk, enum rq_qos_id id,
		const struct rq_qos_ops *ops);
void rq_qos_del(struct rq_qos *rqos);

typedef bool (acquire_inflight_cb_t)(struct rq_wait *rqw, void *private_data);
typedef void (cleanup_cb_t)(struct rq_wait *rqw, void *private_data);

void rq_qos_wait(struct rq_wait *rqw, void *private_data,
		 acquire_inflight_cb_t *acquire_inflight_cb,
		 cleanup_cb_t *cleanup_cb);
bool rq_wait_inc_below(struct rq_wait *rq_wait, unsigned int limit);
bool rq_depth_scale_up(struct rq_depth *rqd);
bool rq_depth_scale_down(struct rq_depth *rqd, bool hard_throttle);
bool rq_depth_calc_max_depth(struct rq_depth *rqd);

#define RQ_QOS_FN(q, fn, ...)	\
	do {	\
		struct rq_qos *rqos;	\
		for_each_rqos(rqos, q)	\
			if (rqos->ops->fn)	\
				rqos->ops->fn(rqos, ##__VA_ARGS__);	\
	} while (0)

static inline void rq_qos_cleanup(struct request_queue *q, struct bio *bio)
{
	if (q->rq_qos)
		RQ_QOS_FN(q, cleanup, bio);
}

static inline void rq_qos_done(struct request_queue *q, struct request *rq)
{
	if (q->rq_qos && !blk_rq_is_passthrough(rq))
		RQ_QOS_FN(q, done, rq);
}

static inline void rq_qos_issue(struct request_queue *q, struct request *rq)
{
	if (q->rq_qos)
		RQ_QOS_FN(q, issue, rq);
}

static inline void rq_qos_requeue(struct request_queue *q, struct request *rq)
{
	if (q->rq_qos)
		RQ_QOS_FN(q, requeue, rq);
}

static inline void rq_qos_done_bio(struct bio *bio)
{
	if (bio->bi_bdev && (bio_flagged(bio, BIO_QOS_THROTTLED) ||
			     bio_flagged(bio, BIO_QOS_MERGED))) {
		struct request_queue *q = bdev_get_queue(bio->bi_bdev);
		if (q->rq_qos)
			RQ_QOS_FN(q, done_bio, bio);
	}
}

static inline void rq_qos_throttle(struct request_queue *q, struct bio *bio)
{
	if (q->rq_qos) {
		bio_set_flag(bio, BIO_QOS_THROTTLED);
		RQ_QOS_FN(q, throttle, bio);
	}
}

static inline void rq_qos_track(struct request_queue *q, struct request *rq,
				struct bio *bio)
{
	if (q->rq_qos)
		RQ_QOS_FN(q, track, rq, bio);
}

static inline void rq_qos_merge(struct request_queue *q, struct request *rq,
				struct bio *bio)
{
	if (q->rq_qos) {
		bio_set_flag(bio, BIO_QOS_MERGED);
		RQ_QOS_FN(q, merge, rq, bio);
	}
}

static inline void rq_qos_queue_depth_changed(struct request_queue *q)
{
	if (q->rq_qos)
		RQ_QOS_FN(q, queue_depth_changed);
}

void rq_qos_exit(struct request_queue *);

#endif
