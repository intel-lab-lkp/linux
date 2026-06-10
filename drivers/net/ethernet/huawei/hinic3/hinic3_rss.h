/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved. */

#ifndef _HINIC3_RSS_H_
#define _HINIC3_RSS_H_

#include <linux/netdevice.h>
#include <linux/ethtool.h>

int hinic3_rss_init(struct net_device *netdev);
void hinic3_rss_uninit(struct net_device *netdev);
void hinic3_try_to_enable_rss(struct net_device *netdev);
void hinic3_clear_rss_config(struct net_device *netdev);

int hinic3_get_rxnfc(struct net_device *netdev,
		     struct ethtool_rxnfc *cmd, u32 *rule_locs);
int hinic3_set_rxnfc(struct net_device *netdev, struct ethtool_rxnfc *cmd);

void hinic3_get_channels(struct net_device *netdev,
			 struct ethtool_channels *channels);
int hinic3_set_channels(struct net_device *netdev,
			struct ethtool_channels *channels);

u32 hinic3_get_rxfh_indir_size(struct net_device *netdev);
u32 hinic3_get_rxfh_key_size(struct net_device *netdev);

int hinic3_get_rxfh(struct net_device *netdev,
		    struct ethtool_rxfh_param *rxfh);
int hinic3_set_rxfh(struct net_device *netdev,
		    struct ethtool_rxfh_param *rxfh,
		    struct netlink_ext_ack *extack);

#endif
