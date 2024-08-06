/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2024 Intel Corporation */

#ifndef __LIBETH_PRIV_H
#define __LIBETH_PRIV_H

#include <linux/types.h>

/* Stats */

struct net_device;

bool libeth_stats_init_priv(struct net_device *dev, u32 rqs, u32 sqs,
			    u32 xdpsqs);
void libeth_stats_free_priv(const struct net_device *dev);

int libeth_stats_get_sset_count(struct net_device *dev);
void libeth_stats_get_strings(struct net_device *dev, u8 *data);
void libeth_stats_get_data(struct net_device *dev, u64 *data);

#endif /* __LIBETH_PRIV_H */
