// SPDX-License-Identifier: GPL-2.0
// Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.

#include "hinic3_rss.h"
#include "hinic3_hwdev.h"
#include "hinic3_nic_dev.h"
#include "hinic3_nic_cfg.h"
#include "hinic3_hwif.h"

void hinic3_clear_rss_config(struct net_device *netdev)
{
	struct hinic3_nic_dev *nic_dev = netdev_priv(netdev);

	kfree(nic_dev->rss_hkey);
	nic_dev->rss_hkey = NULL;

	kfree(nic_dev->rss_indir);
	nic_dev->rss_indir = NULL;
}

void hinic3_try_to_enable_rss(struct net_device *netdev)
{
	/* Completed by later submission due to LoC limit. */
}
