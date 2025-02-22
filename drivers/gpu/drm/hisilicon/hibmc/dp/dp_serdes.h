/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2025 Hisilicon Limited. */

#ifndef DP_SERDES_H
#define DP_SERDES_H

#include "dp_comm.h"

#define HIBMC_DP_HOST_OFFSET		0x10000
#define HIBMC_DP_LANE0_RATE_OFFSET	0x4
#define HIBMC_DP_LANE1_RATE_OFFSET	0xc
#define HIBMC_DP_LANE_STATUS_OFFSET	0x10
#define HIBMC_DP_PMA_LANE0_OFFSET	0x18
#define HIBMC_DP_PMA_LANE1_OFFSET	0x1c
#define HIBMC_DP_HOST_SERDES_CTRL	0x1f001c
#define HIBMC_DP_PMA_TXDEEMPH		GENMASK(18, 1)

/* dp serdes TX-Deempth Configuration */
#define DP_SERDES_VOL0_PRE0		0x280
#define DP_SERDES_VOL0_PRE1		0x2300
#define DP_SERDES_VOL0_PRE2		0x53c0
#define DP_SERDES_VOL0_PRE3		0x8400
#define DP_SERDES_VOL1_PRE0		0x380
#define DP_SERDES_VOL1_PRE1		0x3440
#define DP_SERDES_VOL1_PRE2		0x6480
#define DP_SERDES_VOL2_PRE0		0x500
#define DP_SERDES_VOL2_PRE1		0x4500
#define DP_SERDES_VOL3_PRE0		0x600
#define DP_SERDES_BW_8_1		0x3
#define DP_SERDES_BW_5_4		0x2
#define DP_SERDES_BW_2_7		0x1
#define DP_SERDES_BW_1_62		0x0

#define DP_SERDES_DONE			0x3

int hibmc_dp_serdes_init(struct hibmc_dp_dev *dp);
int hibmc_dp_serdes_rate_switch(u8 rate, struct hibmc_dp_dev *dp);
int hibmc_dp_serdes_set_tx_cfg(struct hibmc_dp_dev *dp, u8 train_set[HIBMC_DP_LANE_NUM_MAX]);

#endif
