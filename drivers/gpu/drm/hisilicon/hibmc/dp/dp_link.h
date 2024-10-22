/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2024 Hisilicon Limited. */

#ifndef DP_LINK_H
#define DP_LINK_H

#include "dp_comm.h"

#define DP_LANE_NUM_MAX		2
#define DP_LANE_STATUS_SIZE	1
#define DP_LANE_NUM_1		0x1
#define DP_LANE_NUM_2		0x2

enum dp_pattern_e {
	DP_PATTERN_NO = 0,
	DP_PATTERN_TPS1,
	DP_PATTERN_TPS2,
	DP_PATTERN_TPS3,
	DP_PATTERN_TPS4,
};

int dp_link_training(struct dp_dev *dp);
u8 dp_get_link_rate(u8 index);

#endif
