// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2024 Intel Corporation */

#include <linux/ethtool.h>

#include <net/libeth/stats.h>
#include <net/netdev_queues.h>

#include "priv.h"

/* Common */

static void libeth_stats_sync(u64 *base, u64 *sarr,
			      const struct u64_stats_sync *syncp,
			      const u64_stats_t *raw, u32 num)
{
	u32 start;

	do {
		start = u64_stats_fetch_begin(syncp);
		for (u32 i = 0; i < num; i++)
			sarr[i] = u64_stats_read(&raw[i]);
	} while (u64_stats_fetch_retry(syncp, start));

	for (u32 i = 0; i < num; i++)
		base[i] += sarr[i];
}

static void __libeth_stats_get_strings(u8 **data, u32 qid, const char *pfx,
				       const char * const *str, u32 num)
{
	for (u32 i = 0; i < num; i++)
		ethtool_sprintf(data, "%s%u_%s", pfx, qid, str[i]);
}

/* The following barely readable compression block defines, amidst others,
 * exported libeth_{rq,sq,xdpsq}_stats_{,de}init() which must be called for
 * each stats container embedded in a queue structure on ifup/ifdown
 * correspondingly. Note that the @qid it takes is the networking stack
 * queue ID, not a driver/device internal one.
 */

#define ___base(s)		aligned_u64	s;
#define ___string(s)		__stringify(s),

#define LIBETH_STATS_DEFINE_HELPERS(pfx, PFX)				      \
struct libeth_##pfx##_base_stats {					      \
	struct mutex		lock;					      \
									      \
	union {								      \
		struct {						      \
			LIBETH_DECLARE_##PFX##_STATS(___base);		      \
		};							      \
		DECLARE_FLEX_ARRAY(aligned_u64, raw);			      \
	};								      \
};									      \
static const char * const libeth_##pfx##_stats_str[] = {		      \
	LIBETH_DECLARE_##PFX##_STATS(___string)				      \
};									      \
static const u32 LIBETH_##PFX##_STATS_NUM =				      \
	ARRAY_SIZE(libeth_##pfx##_stats_str);				      \
									      \
static void libeth_##pfx##_stats_sync(u64 *base,			      \
				      const struct libeth_##pfx##_stats *qs)  \
{									      \
	u64 sarr[ARRAY_SIZE(libeth_##pfx##_stats_str)];			      \
									      \
	if (qs)								      \
		libeth_stats_sync(base, sarr, &qs->syncp, qs->raw,	      \
				  LIBETH_##PFX##_STATS_NUM);		      \
}									      \
									      \
void libeth_##pfx##_stats_init(const struct net_device *dev,		      \
			       struct libeth_##pfx##_stats *stats,	      \
			       u32 qid)					      \
{									      \
	const struct libeth_netdev_priv *priv = netdev_priv(dev);	      \
									      \
	memset(stats, 0, sizeof(*stats));				      \
	u64_stats_init(&stats->syncp);					      \
									      \
	mutex_init(&priv->base_##pfx##s[qid].lock);			      \
	WRITE_ONCE(priv->live_##pfx##s[qid], stats);			      \
}									      \
EXPORT_SYMBOL_NS_GPL(libeth_##pfx##_stats_init, LIBETH);		      \
									      \
void libeth_##pfx##_stats_deinit(const struct net_device *dev, u32 qid)       \
{									      \
	const struct libeth_netdev_priv *priv = netdev_priv(dev);	      \
	struct libeth_##pfx##_base_stats *base = &priv->base_##pfx##s[qid];   \
									      \
	mutex_lock(&base->lock);					      \
	libeth_##pfx##_stats_sync(base->raw,				      \
				  READ_ONCE(priv->live_##pfx##s[qid]));	      \
	mutex_unlock(&base->lock);					      \
									      \
	WRITE_ONCE(priv->live_##pfx##s[qid], NULL);			      \
}									      \
EXPORT_SYMBOL_NS_GPL(libeth_##pfx##_stats_deinit, LIBETH);		      \
									      \
static void libeth_##pfx##_stats_get_strings(u8 **data, u32 num)	      \
{									      \
	for (u32 i = 0; i < num; i++)					      \
		__libeth_stats_get_strings(data, i, #pfx,		      \
					   libeth_##pfx##_stats_str,	      \
					   LIBETH_##PFX##_STATS_NUM);	      \
}									      \
									      \
static void								      \
__libeth_##pfx##_stats_get_data(u64 **data,				      \
				struct libeth_##pfx##_base_stats *base,	      \
				const struct libeth_##pfx##_stats *qs)	      \
{									      \
	mutex_lock(&base->lock);					      \
	memcpy(*data, base->raw, sizeof(*base));			      \
	mutex_unlock(&base->lock);					      \
									      \
	libeth_##pfx##_stats_sync(*data, qs);				      \
	*data += LIBETH_##PFX##_STATS_NUM;				      \
}									      \
									      \
static void								      \
libeth_##pfx##_stats_get_data(u64 **data,				      \
			      const struct libeth_netdev_priv *priv)	      \
{									      \
	for (u32 i = 0; i < priv->last_##pfx##s; i++) {			      \
		const struct libeth_##pfx##_stats *qs;			      \
									      \
		qs = READ_ONCE(priv->live_##pfx##s[i]);			      \
		__libeth_##pfx##_stats_get_data(data,			      \
						&priv->base_##pfx##s[i],      \
						qs);			      \
	}								      \
}

LIBETH_STATS_DEFINE_HELPERS(rq, RQ);
LIBETH_STATS_DEFINE_HELPERS(sq, SQ);
LIBETH_STATS_DEFINE_HELPERS(xdpsq, XDPSQ);

#undef ___base
#undef ___string

/* Netlink stats. Exported fields have the same names as in the NL structs */

struct libeth_stats_export {
	u16	li;
	u16	gi;
};

#define LIBETH_STATS_EXPORT(lpfx, gpfx, field) {			      \
	.li = (offsetof(struct libeth_##lpfx##_stats, field) -		      \
	       offsetof(struct libeth_##lpfx##_stats, raw)) /		      \
	      sizeof_field(struct libeth_##lpfx##_stats, field),	      \
	.gi = offsetof(struct netdev_queue_stats_##gpfx, field) /	      \
	      sizeof_field(struct netdev_queue_stats_##gpfx, field)	      \
}
#define LIBETH_RQ_STATS_EXPORT(field)	LIBETH_STATS_EXPORT(rq, rx, field)
#define LIBETH_SQ_STATS_EXPORT(field)	LIBETH_STATS_EXPORT(sq, tx, field)

static const struct libeth_stats_export libeth_rq_stats_export[] = {
	LIBETH_RQ_STATS_EXPORT(bytes),
	LIBETH_RQ_STATS_EXPORT(packets),
	LIBETH_RQ_STATS_EXPORT(csum_unnecessary),
	LIBETH_RQ_STATS_EXPORT(hw_gro_packets),
	LIBETH_RQ_STATS_EXPORT(hw_gro_bytes),
	LIBETH_RQ_STATS_EXPORT(alloc_fail),
	LIBETH_RQ_STATS_EXPORT(csum_none),
	LIBETH_RQ_STATS_EXPORT(csum_bad),
};

static const struct libeth_stats_export libeth_sq_stats_export[] = {
	LIBETH_SQ_STATS_EXPORT(bytes),
	LIBETH_SQ_STATS_EXPORT(packets),
	LIBETH_SQ_STATS_EXPORT(csum_none),
	LIBETH_SQ_STATS_EXPORT(needs_csum),
	LIBETH_SQ_STATS_EXPORT(hw_gso_packets),
	LIBETH_SQ_STATS_EXPORT(hw_gso_bytes),
	LIBETH_SQ_STATS_EXPORT(stop),
	LIBETH_SQ_STATS_EXPORT(wake),
};

#define libeth_stats_foreach_export(pfx, iter)				      \
	for (const struct libeth_stats_export *iter =			      \
		&libeth_##pfx##_stats_export[0];			      \
	     iter < &libeth_##pfx##_stats_export[			      \
		ARRAY_SIZE(libeth_##pfx##_stats_export)];		      \
	     iter++)

#define LIBETH_STATS_DEFINE_EXPORT(pfx, gpfx)				      \
static void								      \
libeth_get_queue_stats_##gpfx(struct net_device *dev, int idx,		      \
			      struct netdev_queue_stats_##gpfx *stats)	      \
{									      \
	const struct libeth_netdev_priv *priv = netdev_priv(dev);	      \
	const struct libeth_##pfx##_stats *qs;				      \
	u64 *raw = (u64 *)stats;					      \
	u32 start;							      \
									      \
	qs = READ_ONCE(priv->live_##pfx##s[idx]);			      \
	if (!qs)							      \
		return;							      \
									      \
	do {								      \
		start = u64_stats_fetch_begin(&qs->syncp);		      \
									      \
		libeth_stats_foreach_export(pfx, exp)			      \
			raw[exp->gi] = u64_stats_read(&qs->raw[exp->li]);     \
	} while (u64_stats_fetch_retry(&qs->syncp, start));		      \
}									      \
									      \
static void								      \
libeth_get_##pfx##_base_stats(const struct net_device *dev,		      \
			      struct netdev_queue_stats_##gpfx *stats)	      \
{									      \
	const struct libeth_netdev_priv *priv = netdev_priv(dev);	      \
	u64 *raw = (u64 *)stats;					      \
									      \
	memset(stats, 0, sizeof(*(stats)));				      \
									      \
	for (u32 i = 0; i < dev->num_##gpfx##_queues; i++) {		      \
		struct libeth_##pfx##_base_stats *base =		      \
			&priv->base_##pfx##s[i];			      \
									      \
		mutex_lock(&base->lock);				      \
									      \
		libeth_stats_foreach_export(pfx, exp)			      \
			raw[exp->gi] += base->raw[exp->li];		      \
									      \
		mutex_unlock(&base->lock);				      \
	}								      \
}

LIBETH_STATS_DEFINE_EXPORT(rq, rx);
LIBETH_STATS_DEFINE_EXPORT(sq, tx);

static void libeth_get_base_stats(struct net_device *dev,
				  struct netdev_queue_stats_rx *rx,
				  struct netdev_queue_stats_tx *tx)
{
	libeth_get_rq_base_stats(dev, rx);
	libeth_get_sq_base_stats(dev, tx);
}

static const struct netdev_stat_ops libeth_netdev_stat_ops = {
	.get_base_stats		= libeth_get_base_stats,
	.get_queue_stats_rx	= libeth_get_queue_stats_rx,
	.get_queue_stats_tx	= libeth_get_queue_stats_tx,
};

/* Ethtool: base + live */

int libeth_stats_get_sset_count(struct net_device *dev)
{
	struct libeth_netdev_priv *priv = netdev_priv(dev);

	priv->last_rqs = dev->real_num_rx_queues;
	priv->last_sqs = dev->real_num_tx_queues;
	priv->last_xdpsqs = priv->curr_xdpsqs;

	return priv->last_rqs * LIBETH_RQ_STATS_NUM +
	       priv->last_sqs * LIBETH_SQ_STATS_NUM +
	       priv->last_xdpsqs * LIBETH_XDPSQ_STATS_NUM;
}

void libeth_stats_get_strings(struct net_device *dev, u8 *data)
{
	const struct libeth_netdev_priv *priv = netdev_priv(dev);

	libeth_rq_stats_get_strings(&data, priv->last_rqs);
	libeth_sq_stats_get_strings(&data, priv->last_sqs);
	libeth_xdpsq_stats_get_strings(&data, priv->last_xdpsqs);
}

void libeth_stats_get_data(struct net_device *dev, u64 *data)
{
	struct libeth_netdev_priv *priv = netdev_priv(dev);

	libeth_rq_stats_get_data(&data, priv);
	libeth_sq_stats_get_data(&data, priv);
	libeth_xdpsq_stats_get_data(&data, priv);

	priv->last_rqs = 0;
	priv->last_sqs = 0;
	priv->last_xdpsqs = 0;
}

/* Private init.
 * All the structures getting allocated here are slowpath, so NUMA-aware
 * allocation is not required for them.
 */

bool libeth_stats_init_priv(struct net_device *dev, u32 rqs, u32 sqs,
			    u32 xdpsqs)
{
	struct libeth_netdev_priv *priv = netdev_priv(dev);

	priv->base_rqs = kvcalloc(rqs, sizeof(*priv->base_rqs), GFP_KERNEL);
	if (!priv->base_rqs)
		return false;

	priv->live_rqs = kvcalloc(rqs, sizeof(*priv->live_rqs), GFP_KERNEL);
	if (!priv->live_rqs)
		goto err_base_rqs;

	priv->base_sqs = kvcalloc(sqs, sizeof(*priv->base_sqs), GFP_KERNEL);
	if (!priv->base_sqs)
		goto err_live_rqs;

	priv->live_sqs = kvcalloc(sqs, sizeof(*priv->live_sqs), GFP_KERNEL);
	if (!priv->live_sqs)
		goto err_base_sqs;

	dev->stat_ops = &libeth_netdev_stat_ops;

	if (!xdpsqs)
		return true;

	priv->base_xdpsqs = kvcalloc(xdpsqs, sizeof(*priv->base_xdpsqs),
				     GFP_KERNEL);
	if (!priv->base_xdpsqs)
		goto err_live_sqs;

	priv->live_xdpsqs = kvcalloc(xdpsqs, sizeof(*priv->live_xdpsqs),
				     GFP_KERNEL);
	if (!priv->live_xdpsqs)
		goto err_base_xdpsqs;

	priv->max_xdpsqs = xdpsqs;

	return true;

err_base_xdpsqs:
	kvfree(priv->base_xdpsqs);
err_live_sqs:
	kvfree(priv->live_sqs);
err_base_sqs:
	kvfree(priv->base_sqs);
err_live_rqs:
	kvfree(priv->live_rqs);
err_base_rqs:
	kvfree(priv->base_rqs);

	return false;
}

void libeth_stats_free_priv(const struct net_device *dev)
{
	const struct libeth_netdev_priv *priv = netdev_priv(dev);

	kvfree(priv->base_rqs);
	kvfree(priv->live_rqs);
	kvfree(priv->base_sqs);
	kvfree(priv->live_sqs);

	if (!priv->max_xdpsqs)
		return;

	kvfree(priv->base_xdpsqs);
	kvfree(priv->live_xdpsqs);
}
