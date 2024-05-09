// SPDX-License-Identifier: GPL-2.0
/*
 * Per-node request latency tracking.
 *
 * Copyright (C) 2023 Hannes Reinecke
 *
 * A simple per-node latency tracker for use by I/O scheduler.
 * Latencies are measures over 'win_usec' microseconds and stored per node.
 * If the number of measurements falls below 'lowat' the measurement is
 * assumed to be unreliable and will become 'stale'.
 * These 'stale' latencies can be 'decayed', where during each measurement
 * interval the 'stale' latency value is decreased by 'decay' percent.
 * Once the 'stale' latency reaches zero it will be updated by the
 * measured latency.
 */
#include <linux/kernel.h>
#include <linux/blk_types.h>
#include <linux/slab.h>

#include "blk-stat.h"
#include "blk-rq-qos.h"
#include "blk.h"

#define NLAT_DEFAULT_LOWAT 2
#define NLAT_DEFAULT_DECAY 50

struct rq_nlat {
	struct rq_qos rqos;

	u64 win_usec;		/* latency measurement window in microseconds */
	unsigned int lowat;	/* Low Watermark latency measurement */
	unsigned int decay;	/* Percentage for 'decaying' latencies */
	bool enabled;

	struct blk_stat_callback *cb;

	unsigned int num;
	u64 *latency;
	unsigned int *samples;
};

static inline struct rq_nlat *RQNLAT(struct rq_qos *rqos)
{
	return container_of(rqos, struct rq_nlat, rqos);
}

static u64 nlat_default_latency_usec(struct request_queue *q)
{
	/*
	 * We default to 2msec for non-rotational storage, and 75msec
	 * for rotational storage.
	 */
	if (blk_queue_nonrot(q))
		return 2000ULL;
	else
		return 75000ULL;
}

static void nlat_timer_fn(struct blk_stat_callback *cb)
{
	struct rq_nlat *nlat = cb->data;
	int n;

	for (n = 0; n < cb->buckets; n++) {
		if (cb->stat[n].nr_samples < nlat->lowat) {
			/*
			 * 'decay' the latency by the specified
			 * percentage to ensure the queues are
			 * being tested to balance out temporary
			 * latency spikes.
			 */
			nlat->latency[n] =
				div64_u64(nlat->latency[n] * nlat->decay, 100);
		} else
			nlat->latency[n] = cb->stat[n].mean;
		nlat->samples[n] = cb->stat[n].nr_samples;
	}
	if (nlat->enabled)
		blk_stat_activate_nsecs(nlat->cb, nlat->win_usec * 1000);
}

static int nlat_bucket_node(const struct request *rq)
{
	if (!rq->mq_ctx)
		return -1;
	return cpu_to_node(blk_mq_rq_cpu((struct request *)rq));
}

static void nlat_exit(struct rq_qos *rqos)
{
	struct rq_nlat *nlat = RQNLAT(rqos);

	blk_stat_remove_callback(nlat->rqos.disk->queue, nlat->cb);
	blk_stat_free_callback(nlat->cb);
	kfree(nlat->samples);
	kfree(nlat->latency);
	kfree(nlat);
}

#ifdef CONFIG_BLK_DEBUG_FS
static int nlat_win_usec_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct rq_nlat *nlat = RQNLAT(rqos);

	seq_printf(m, "%llu\n", nlat->win_usec);
	return 0;
}

static ssize_t nlat_win_usec_write(void *data, const char __user *buf,
			size_t count, loff_t *ppos)
{
	struct rq_qos *rqos = data;
	struct rq_nlat *nlat = RQNLAT(rqos);
	char val[16] = { };
	u64 usec;
	int err;

	if (blk_queue_dying(nlat->rqos.disk->queue))
		return -ENOENT;

	if (count >= sizeof(val))
		return -EINVAL;

	if (copy_from_user(val, buf, count))
		return -EFAULT;

	err = kstrtoull(val, 10, &usec);
	if (err)
		return err;
	blk_stat_deactivate(nlat->cb);
	nlat->win_usec = usec;
	blk_stat_activate_nsecs(nlat->cb, nlat->win_usec * 1000);

	return count;
}

static int nlat_lowat_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct rq_nlat *nlat = RQNLAT(rqos);

	seq_printf(m, "%u\n", nlat->lowat);
	return 0;
}

static ssize_t nlat_lowat_write(void *data, const char __user *buf,
			size_t count, loff_t *ppos)
{
	struct rq_qos *rqos = data;
	struct rq_nlat *nlat = RQNLAT(rqos);
	char val[16] = { };
	unsigned int lowat;
	int err;

	if (blk_queue_dying(nlat->rqos.disk->queue))
		return -ENOENT;

	if (count >= sizeof(val))
		return -EINVAL;

	if (copy_from_user(val, buf, count))
		return -EFAULT;

	err = kstrtouint(val, 10, &lowat);
	if (err)
		return err;
	blk_stat_deactivate(nlat->cb);
	nlat->lowat = lowat;
	blk_stat_activate_nsecs(nlat->cb, nlat->win_usec * 1000);

	return count;
}

static int nlat_decay_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct rq_nlat *nlat = RQNLAT(rqos);

	seq_printf(m, "%u\n", nlat->decay);
	return 0;
}

static ssize_t nlat_decay_write(void *data, const char __user *buf,
			size_t count, loff_t *ppos)
{
	struct rq_qos *rqos = data;
	struct rq_nlat *nlat = RQNLAT(rqos);
	char val[16] = { };
	unsigned int decay;
	int err;

	if (blk_queue_dying(nlat->rqos.disk->queue))
		return -ENOENT;

	if (count >= sizeof(val))
		return -EINVAL;

	if (copy_from_user(val, buf, count))
		return -EFAULT;

	err = kstrtouint(val, 10, &decay);
	if (err)
		return err;
	if (decay > 100)
		return -EINVAL;
	blk_stat_deactivate(nlat->cb);
	nlat->decay = decay;
	blk_stat_activate_nsecs(nlat->cb, nlat->win_usec * 1000);

	return count;
}

static int nlat_enabled_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct rq_nlat *nlat = RQNLAT(rqos);

	seq_printf(m, "%d\n", nlat->enabled);
	return 0;
}

static int nlat_id_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;

	seq_printf(m, "%u\n", rqos->id);
	return 0;
}

static int nlat_latency_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct rq_nlat *nlat = RQNLAT(rqos);
	int n;

	if (!nlat->enabled)
		return 0;

	for (n = 0; n < nlat->num; n++) {
		if (n > 0)
			seq_puts(m, " ");
		seq_printf(m, "%llu", nlat->latency[n]);
	}
	seq_puts(m, "\n");
	return 0;
}

static int nlat_samples_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct rq_nlat *nlat = RQNLAT(rqos);
	int n;

	if (!nlat->enabled)
		return 0;

	for (n = 0; n < nlat->num; n++) {
		if (n > 0)
			seq_puts(m, " ");
		seq_printf(m, "%u", nlat->samples[n]);
	}
	seq_puts(m, "\n");
	return 0;
}

static const struct blk_mq_debugfs_attr nlat_debugfs_attrs[] = {
	{"win_usec", 0600, nlat_win_usec_show, nlat_win_usec_write},
	{"lowat", 0600, nlat_lowat_show, nlat_lowat_write},
	{"decay", 0600, nlat_decay_show, nlat_decay_write},
	{"enabled", 0400, nlat_enabled_show},
	{"id", 0400, nlat_id_show},
	{"latency", 0400, nlat_latency_show},
	{"samples", 0400, nlat_samples_show},
	{},
};
#endif

static const struct rq_qos_ops nlat_rqos_ops = {
	.exit = nlat_exit,
#ifdef CONFIG_BLK_DEBUG_FS
	.debugfs_attrs = nlat_debugfs_attrs,
#endif
};

u64 blk_nlat_latency(struct gendisk *disk, int node)
{
	struct rq_qos *rqos;
	struct rq_nlat *nlat;

	rqos = nlat_rq_qos(disk->queue);
	if (!rqos)
		return 0;
	nlat = RQNLAT(rqos);
	if (node > nlat->num)
		return 0;

	return div64_u64(nlat->latency[node], 1000);
}
EXPORT_SYMBOL_GPL(blk_nlat_latency);

int blk_nlat_enable(struct gendisk *disk)
{
	struct rq_qos *rqos;
	struct rq_nlat *nlat;

	/* Latency tracking not enabled? */
	rqos = nlat_rq_qos(disk->queue);
	if (!rqos)
		return -EINVAL;
	nlat = RQNLAT(rqos);
	if (nlat->enabled)
		return 0;

	/* Queue not registered? Maybe shutting down... */
	if (!blk_queue_registered(disk->queue))
		return -EAGAIN;

	nlat->enabled = true;
	memset(nlat->latency, 0, sizeof(u64) * nlat->num);
	memset(nlat->samples, 0, sizeof(unsigned int) * nlat->num);
	blk_stat_activate_nsecs(nlat->cb, nlat->win_usec * 1000);

	return 0;
}
EXPORT_SYMBOL_GPL(blk_nlat_enable);

void blk_nlat_disable(struct gendisk *disk)
{
	struct rq_qos *rqos = nlat_rq_qos(disk->queue);
	struct rq_nlat *nlat;

	if (!rqos)
		return;
	nlat = RQNLAT(rqos);
	if (nlat->enabled) {
		blk_stat_deactivate(nlat->cb);
		nlat->enabled = false;
	}
}
EXPORT_SYMBOL_GPL(blk_nlat_disable);

int blk_nlat_init(struct gendisk *disk)
{
	struct rq_nlat *nlat;
	int ret = -ENOMEM;

	nlat = kzalloc(sizeof(*nlat), GFP_KERNEL);
	if (!nlat)
		return -ENOMEM;

	nlat->num = num_possible_nodes();
	nlat->lowat = NLAT_DEFAULT_LOWAT;
	nlat->decay = NLAT_DEFAULT_DECAY;
	nlat->win_usec = nlat_default_latency_usec(disk->queue);

	nlat->latency = kcalloc(nlat->num, sizeof(u64), GFP_KERNEL);
	if (!nlat->latency)
		goto err_free;
	nlat->samples = kcalloc(nlat->num, sizeof(unsigned int), GFP_KERNEL);
	if (!nlat->samples)
		goto err_free;
	nlat->cb = blk_stat_alloc_callback(nlat_timer_fn, nlat_bucket_node,
					   nlat->num, nlat);
	if (!nlat->cb)
		goto err_free;

	/*
	 * Assign rwb and add the stats callback.
	 */
	mutex_lock(&disk->queue->rq_qos_mutex);
	ret = rq_qos_add(&nlat->rqos, disk, RQ_QOS_NLAT, &nlat_rqos_ops);
	mutex_unlock(&disk->queue->rq_qos_mutex);
	if (ret)
		goto err_free_cb;

	blk_stat_add_callback(disk->queue, nlat->cb);

	return 0;

err_free_cb:
	blk_stat_free_callback(nlat->cb);
err_free:
	kfree(nlat->samples);
	kfree(nlat->latency);
	kfree(nlat);
	return ret;
}
EXPORT_SYMBOL_GPL(blk_nlat_init);
