/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_RSS_H
#define HINIC3_RSS_H

#include <linux/netdevice.h>

int hinic3_rss_init(struct net_device *netdev);
void hinic3_rss_deinit(struct net_device *netdev);
void hinic3_try_to_enable_rss(struct net_device *netdev);
void hinic3_clear_rss_config(struct net_device *netdev);

#endif
