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

/* I3C read transfer function */
static int sbtsi_i3c_read(struct sbtsi_data *data, u8 reg, u8 *val)
{
	struct i3c_xfer xfers[2] = { };

	/* Add Register data to read/write */
	xfers[0].rnw = false;
	xfers[0].len = 1;
	xfers[0].data.out = &reg;

	xfers[1].rnw = true;
	xfers[1].len = 1;
	xfers[1].data.in = val;

	return i3c_device_do_xfers(data->i3cdev, xfers, 2, I3C_SDR);
}

/* I3C write transfer function */
static int sbtsi_i3c_write(struct sbtsi_data *data, u8 reg, u8 val)
{
	u8 buf[2];
	struct i3c_xfer xfers = {
		.rnw = false,
		.len = 2,
		.data.out = buf,
	};

	buf[0] = reg;
	buf[1] = val;

	return i3c_device_do_xfers(data->i3cdev, &xfers, 1, I3C_SDR);
}

/* Unified transfer function for I2C and I3C access */
int sbtsi_xfer(struct sbtsi_data *data, u8 reg, u8 *val, bool is_read)
{
	if (data->is_i3c)
		return is_read ? sbtsi_i3c_read(data, reg, val)
			       : sbtsi_i3c_write(data, reg, *val);

	return sbtsi_i2c_xfer(data, reg, val, is_read);
}
EXPORT_SYMBOL_GPL(sbtsi_xfer);
