/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2024 Hisilicon Limited. */

#ifndef DP_AUX_H
#define DP_AUX_H

#include <linux/bitops.h>
#include "dp_comm.h"

#define DP_I2C_MOT_W			0x4
#define DP_I2C_MOT_R			0x5
#define DP_NATIVE_W			0x8
#define DP_NATIVE_R			0x9

#define AUX_I2C_WRITE_SUCCESS		0x1
#define AUX_I2C_WRITE_PARTIAL_SUCCESS	0x2

#define EQ_MAX_RETRY			5
#define AUX_RW_MAX_RETRY		7

#define DPCD_LINK_BW_SET		0x0100
#define DPCD_LANE_COUNT_SET		0x0101
#define DPCD_TRAINING_PATTERN_SET	0x0102
#define DPCD_TRAINING_LANE0_SET		0x0103
#define DPCD_DOWNSPREAD_CTRL		0x0107
#define DPCD_LANE0_1_STATUS		0x0202
#define DPCD_ADJUST_REQUEST_LANE0_1	0x0206

#define DPCD_VOLTAGE_SWING_LANE_0	(BIT(0) | BIT(1))
#define DPCD_PRE_EMPHASIS_LANE_0	(BIT(2) | BIT(3))
#define DPCD_VOLTAGE_SWING_SET_S	0
#define DPCD_PRE_EMPHASIS_SET_S		3
#define DPCD_SCRAMBLING_DISABLE		BIT(5)
#define DPCD_CR_DONE_BITS		BIT(0)
#define DPCD_EQ_DONE_BITS		(BIT(0) | BIT(1) | BIT(2))
#define DPCD_ENHANCED_FRAME_EN		0x80

#define DPCD_TRAINING_PATTERN_DISABLE	0x0
#define DPCD_TRAINING_PATTERN_1		0x1
#define DPCD_TRAINING_PATTERN_2		0x2
#define DPCD_TRAINING_PATTERN_3		0x3
#define DPCD_TRAINING_PATTERN_4		0x7
#define DPCD_VOLTAGE_SWING_LEVEL_0	FIELD_PREP(GENMASK(1, 0), 0)
#define DPCD_VOLTAGE_SWING_LEVEL_1	FIELD_PREP(GENMASK(1, 0), 1)
#define DPCD_VOLTAGE_SWING_LEVEL_2	FIELD_PREP(GENMASK(1, 0), 2)
#define DPCD_VOLTAGE_SWING_LEVEL_3	FIELD_PREP(GENMASK(1, 0), 3)
#define DPCD_PRE_EMPHASIS_LEVEL_0	FIELD_PREP(GENMASK(4, 3), 0)
#define DPCD_PRE_EMPHASIS_LEVEL_1	FIELD_PREP(GENMASK(4, 3), 1)
#define DPCD_PRE_EMPHASIS_LEVEL_2	FIELD_PREP(GENMASK(4, 3), 2)
#define DPCD_PRE_EMPHASIS_LEVEL_3	FIELD_PREP(GENMASK(4, 3), 3)

#define DP_LINK_RATE_NUM		4
#define DP_LINK_RATE_0			0x6
#define DP_LINK_RATE_1			0xA
#define DP_LINK_RATE_2			0x14
#define DP_LINK_RATE_3			0x1E
#define DP_AUX_NATIVE_REPLY_MASK	(0x3 << 4)
#define DP_AUX_ACK			(0 << 4)
#define DP_AUX_NACK			(0x1 << 4)
#define DP_AUX_DEFER			(0x2 << 4)
#define DP_CFG_AUX_S			17
#define DP_CFG_AUX_STATUS_S		4

#define AUX_4_BYTE			4
#define AUX_4_BIT			4
#define AUX_8_BIT			8
#define AUX_RESET_INTERVAL		15
#define AUX_RETRY_INTERVAL		500
#define AUX_READY_DATA_BYTE_S		12

/* aux_cmd_addr register shift */
#define AUX_CMD_REQ_TYPE_S		0
#define AUX_CMD_REQ_LEN_S		4
#define AUX_CMD_ADDR_S			8
#define AUX_CMD_I2C_ADDR_ONLY_S		28

int dp_aux_write(struct hibmc_dp_dev *dp, u32 address, u8 *buffer, u8 size);
int dp_aux_read(struct hibmc_dp_dev *dp, u32 address, u8 *buffer, u8 size);

#endif
