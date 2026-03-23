/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#ifndef _TSI_CORE_H_
#define _TSI_CORE_H_

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/types.h>
#include <linux/i3c/device.h>
#include <linux/i3c/master.h>

/* Each client has this additional data */
struct sbtsi_data {
	union {
		struct i2c_client *client;
		struct i3c_device *i3cdev;
	};
	bool ext_range_mode;
	bool read_order;
	bool is_i3c;
};

int sbtsi_xfer(struct sbtsi_data *data, u8 reg, u8 *val, bool is_read);

#ifdef CONFIG_AMD_SBTSI_HWMON
int create_sbtsi_hwmon_sensor_device(struct device *dev, struct sbtsi_data *data);
#else
static inline int create_sbtsi_hwmon_sensor_device(struct device *dev, struct sbtsi_data *data)
{
	return 0;
}
#endif
#endif /*_TSI_CORE_H_*/
