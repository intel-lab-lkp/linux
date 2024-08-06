/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2024 Intel Corporation */

#ifndef __LIBETH_NETDEV_H
#define __LIBETH_NETDEV_H

#include <linux/types.h>

struct ethtool_stats;

struct net_device *__libeth_netdev_alloc(u32 priv, u32 rqs, u32 sqs,
					 u32 xdpsqs);
void libeth_netdev_free(struct net_device *dev);

int __libeth_set_real_num_queues(struct net_device *dev, u32 rqs, u32 sqs,
				 u32 xdpsqs);

#define libeth_netdev_alloc(priv, rqs, sqs, ...)			\
	__libeth_netdev_alloc(priv, rqs, sqs, (__VA_ARGS__ + 0))
#define libeth_set_real_num_queues(dev, rqs, sqs, ...)			\
	__libeth_set_real_num_queues(dev, rqs, sqs, (__VA_ARGS__ + 0))

/* Ethtool */

int libeth_ethtool_get_sset_count(struct net_device *dev, int sset);
void libeth_ethtool_get_strings(struct net_device *dev, u32 sset, u8 *data);
void libeth_ethtool_get_stats(struct net_device *dev,
			      struct ethtool_stats *stats,
			      u64 *data);

#endif /* __LIBETH_NETDEV_H */
