/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2024 Hisilicon Limited. */

#ifndef DP_AUX_H
#define DP_AUX_H

#include <linux/bitops.h>
#include "dp_comm.h"

#define AUX_I2C_WRITE_SUCCESS		0x1
#define AUX_I2C_WRITE_PARTIAL_SUCCESS	0x2

#define EQ_MAX_RETRY			5

#define DP_CFG_AUX_S			17
#define DP_CFG_AUX_STATUS_S		4

#define AUX_4_BYTE			4
#define AUX_4_BIT			4
#define AUX_8_BIT			8

#define AUX_READY_DATA_BYTE_S		12

/* aux_cmd_addr register shift */
#define AUX_CMD_REQ_LEN_S		4
#define AUX_CMD_ADDR_S			8
#define AUX_CMD_I2C_ADDR_ONLY_S		28

void dp_aux_init(struct dp_dev *dp);

#endif
