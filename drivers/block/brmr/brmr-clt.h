/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Block device over RMR (BRMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#ifndef BRMR_PRI_H
#define BRMR_PRI_H

#include <linux/limits.h>
#include <linux/blk-mq.h>
#include "rmr-pool.h"

#include "brmr-proto.h"

#define BRMR_VER_MAJOR 0
#define BRMR_VER_MINOR 1

#ifndef BRMR_VER_STRING
#define BRMR_VER_STRING __stringify(BRMR_VER_MAJOR) "." \
			 __stringify(BRMR_VER_MINOR)
#endif

#define BRMR_LINK_NAME "block"

#ifdef CONFIG_ARCH_NO_SG_CHAIN
#define BRMR_INLINE_SG_CNT 0
#else
#define BRMR_INLINE_SG_CNT 2
#endif
#define BRMR_RDMA_SGL_SIZE (sizeof(struct scatterlist) * BRMR_INLINE_SG_CNT)

enum brmr_dev_state {
	DEV_STATE_INIT,
	DEV_STATE_READY,
	DEV_STATE_DISCONNECTED,
	DEV_STATE_CLOSING,
};

struct brmr_clt_iu {
	struct request		*rq;
	struct rmr_iu		*rmr_iu;
	struct brmr_clt_dev	*dev;
	blk_status_t		status;
	struct sg_table		sgt;
	struct scatterlist	sgl[];
};

struct brmr_queue {
	struct list_head	requeue_list;
	unsigned long		in_list;
	struct blk_mq_hw_ctx	*hctx;
};

struct brmr_cpu_qlist {
	struct list_head	requeue_list;
	spinlock_t		requeue_lock;
	unsigned int		cpu;
};

struct brmr_clt_pool {
	struct list_head        list;
	struct rmr_pool         *rmr;
	wait_queue_head_t       rmr_waitq;
	bool                    rmr_ready;
	int			queue_depth;
	u32			max_io_size;
	u32			chunk_size;
	u32			max_segments;
	struct brmr_cpu_qlist __percpu
				*cpu_queues;
	DECLARE_BITMAP(cpu_queues_bm, NR_CPUS);
	int	__percpu	*cpu_rr; /* per-cpu var for CPU round-robin */
	atomic_t	     	busy;
	struct blk_mq_tag_set	tag_set;
	struct mutex		lock; /* protects state and devs_list */
	struct list_head        devs_list; /* list of struct brmr_clt_dev */
	refcount_t		refcount;
	char			poolname[NAME_MAX];
};

/**
 * Statistic of requests submitted to the rmr-clt layer.
 * This means total number of requests received from blk
 * is cnt_whole+(cnt_split/2)
 * while total number submitted to rmr-clt is cnt_whole+cnt_split
 */
struct brmr_stats_rq {
	struct {
		u64 cnt_whole;
		u64 cnt_split;
		u64 total_sectors;
	} dir[2];
};

#define STATS_SIZES_NUM 16

struct brmr_stats_sizes {
	struct {
		u64 cnt_whole[STATS_SIZES_NUM];
		u64 cnt_left[STATS_SIZES_NUM];
		u64 cnt_right[STATS_SIZES_NUM];
	} dir[2];
};

struct brmr_stats_sts_resource {
	u64 get_iu;
	u64 get_iu2;
	u64 clt_request1;
	u64 clt_request;
};

struct brmr_stats_pcpu {

	struct brmr_stats_rq submitted_requests;
	struct brmr_stats_sizes request_sizes;
	struct brmr_stats_sts_resource sts_resource;
};

struct brmr_clt_stats {
	struct brmr_stats_pcpu __percpu *pcpu_stats;
};

struct brmr_clt_dev {
	struct brmr_clt_pool	*pool;
	struct request_queue	*queue;
	struct brmr_queue	*hw_queues;
	u32			idx;
	enum brmr_dev_state	dev_state;
	bool			read_only;
	bool			map_incomplete;
	u64			size_sect;	/* device size in sectors */
	struct list_head        list;
	struct brmr_clt_stats	stats;
	struct gendisk		*gd;
	struct kobject		kobj;
	struct kobject		kobj_stats;
	char			blk_symlink_name[NAME_MAX];
	refcount_t		refcount;
	struct work_struct	unmap_on_rmmod_work;
	bool			wc;
	bool			fua;

	/*
	 * Params holding block device related info
	 */
	u32	max_hw_sectors;
	u32	max_write_zeroes_sectors;
	u32	max_discard_sectors;
	u32	discard_granularity;
	u32	discard_alignment;
	u16	physical_block_size;
	u16	logical_block_size;
	u16	max_segments;
	u16	secure_discard;
	u8	cache_policy;
};

#define BRMR_HEADER_MAGIC_TOKEN 0x312631494f4e4f53

#define BRMR_HEADER_VERSION_INITIAL 1
#define BRMR_CURRENT_HEADER_VERSION BRMR_HEADER_VERSION_INITIAL

static inline enum rmr_io_flags rq_to_rmr_flags(struct request *rq)
{
	enum rmr_io_flags rmr_flag;

	switch (req_op(rq)) {
	case REQ_OP_READ:
		rmr_flag = RMR_OP_READ;
		break;
	case REQ_OP_WRITE:
		rmr_flag = RMR_OP_WRITE;
		break;
	case REQ_OP_DISCARD:
		rmr_flag = RMR_OP_DISCARD;
		break;
	case REQ_OP_WRITE_ZEROES:
		rmr_flag = RMR_OP_WRITE_ZEROES;
		break;
	case REQ_OP_FLUSH:
		rmr_flag = RMR_OP_FLUSH;
		break;
/* TODO
	case REQ_OP_SECURE_ERASE:
		rmr_flag = IBNBD_OP_SECURE_ERASE;
		break;
*/
	default:
		WARN(1, "Unknown request type %d (flags %u)\n",
		     req_op(rq), rq->cmd_flags);
		rmr_flag = 0;
	}

	/* Set sync flag for write request. */
	if (op_is_sync(rq->cmd_flags))
		rmr_flag |= RMR_F_SYNC;

	if (op_is_flush(rq->cmd_flags))
		rmr_flag |= RMR_F_FUA;

	return rmr_flag;
}

static inline u32 brmr_pool_chunk_size(struct brmr_clt_pool *pool)
{
	return pool->chunk_size;
}

struct brmr_clt_dev *brmr_clt_map_device(const char *pool, u64 size);
int brmr_clt_close_device(struct brmr_clt_dev *dev, const struct attribute *sysfs_self);

void brmr_clt_put_dev(struct brmr_clt_dev *dev);

struct brmr_clt_dev *find_and_get_device(const char *name);

/* brmr-sysfs.c */

int brmr_clt_create_sysfs_files(void);
void brmr_clt_destroy_sysfs_files(void);

void brmr_clt_destroy_dev_sysfs_files(struct brmr_clt_dev *dev,
				      const struct attribute *sysfs_self);

/* brmr-reque.c */

bool brmr_add_to_requeue(struct brmr_clt_pool *pool, struct brmr_queue *q);
void brmr_requeue_requests(struct brmr_clt_pool *pool);
void brmr_init_cpu_qlists(struct brmr_cpu_qlist __percpu *cpu_queues);

/* brmr-stats.c */

int brmr_clt_init_stats(struct brmr_clt_stats *stats);
void brmr_clt_free_stats(struct brmr_clt_stats *stats);

int brmr_clt_reset_submitted_req(struct brmr_clt_stats *stats, bool enable);
int brmr_clt_reset_req_sizes(struct brmr_clt_stats *stats, bool enable);
int brmr_clt_reset_sts_resource(struct brmr_clt_stats *stats, bool enable);

/**
 * size: size of the request submitted in bytes
 * split: 0 when request from blk is submitted to rmr-clt as 1
 * 1 if it is one part of the split from a blk request
 */
void brmr_update_stats(struct brmr_clt_stats *stats, size_t size, int split, int d);

/**
 * which: at which place is BLK_STS_RESOURCE returned?
 */
void brmr_clt_update_sts_resource(struct brmr_clt_stats *stats, int which);

ssize_t brmr_clt_stats_sizes_to_str(struct brmr_clt_stats *stats, char *page, size_t len);

ssize_t brmr_clt_stats_rq_to_str(struct brmr_clt_stats *stats, char *page, size_t len);

ssize_t brmr_stats_sts_resource_to_str(
	struct brmr_clt_stats *stats, char *page, size_t len);

ssize_t brmr_stats_sts_resource_per_cpu_to_str(
	struct brmr_clt_stats *stats, char *page, size_t len);

#define STAT_STORE_FUNC(type, store, reset)				\
static ssize_t store##_store(struct kobject *kobj,			\
			     struct kobj_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	int ret = -EINVAL;						\
	type *dev = container_of(kobj, type, kobj_stats);		\
									\
	if (sysfs_streq(buf, "1"))					\
		ret = reset(&dev->stats, true);				\
	else if (sysfs_streq(buf, "0"))					\
		ret = reset(&dev->stats, false);			\
	if (ret)							\
		return ret;						\
									\
	return count;							\
}

#define STAT_SHOW_FUNC(type, show, print)				\
static ssize_t show##_show(struct kobject *kobj,			\
			   struct kobj_attribute *attr,			\
			   char *page)					\
{									\
	type *dev = container_of(kobj, type, kobj_stats);		\
									\
	return print(&dev->stats, page, PAGE_SIZE);			\
}

#define STAT_ATTR(type, stat, print, reset)				\
STAT_STORE_FUNC(type, stat, reset)					\
STAT_SHOW_FUNC(type, stat, print)					\
static struct kobj_attribute stat##_attr =				\
		__ATTR(stat, 0644,					\
		       stat##_show,					\
		       stat##_store)

#endif /* BRMR_PRI_H */
