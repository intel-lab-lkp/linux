/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2024 Hisilicon Limited. */

#ifndef DP_AUX_H
#define DP_AUX_H

#include <linux/bitops.h>
#include "dp_comm.h"

#define AUX_I2C_WRITE_SUCCESS		0x1
#define AUX_I2C_WRITE_PARTIAL_SUCCESS	0x2
#define EQ_MAX_RETRY			5
#define BYTES_IN_U32			4
#define BITS_IN_U8			8

/* aux_cmd_addr register shift */
#define AUX_CMD_REQ_LEN			GENMASK(7, 4)
#define AUX_CMD_ADDR			GENMASK(27, 8)
#define AUX_CMD_I2C_ADDR_ONLY		BIT(28)

void hibmc_dp_aux_init(struct dp_dev *dp);

#endif
