// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tsi-core.c - file defining SB-TSI protocols compliant
 *              AMD SoC device.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#include <linux/module.h>
#include <linux/misc/tsi.h>

/* I2C transfer function */
static int sbtsi_i2c_xfer(struct sbtsi_data *data, u8 reg, u8 *val, bool is_read)
{
	if (is_read) {
		int ret = i2c_smbus_read_byte_data(data->client, reg);

		if (ret < 0)
			return ret;
		*val = ret;
		return 0;
	}
	return i2c_smbus_write_byte_data(data->client, reg, *val);
}

int sbtsi_xfer(struct sbtsi_data *data, u8 reg, u8 *val, bool is_read)
{
	return sbtsi_i2c_xfer(data, reg, val, is_read);
}
EXPORT_SYMBOL_GPL(sbtsi_xfer);
