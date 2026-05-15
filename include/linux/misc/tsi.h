/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AMD SBTSI shared data structure and auxiliary bus definitions.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#ifndef _LINUX_TSI_H_
#define _LINUX_TSI_H_

#include <linux/i2c.h>
#include <linux/types.h>

/**
 * struct sbtsi_data - driver private data for an AMD SB-TSI device
 * @client:	underlying I2C client
 * @ext_range_mode:	sensor uses extended temperature range
 * @read_order:	if set, decimal part must be read before integer part
 */
struct sbtsi_data {
	struct i2c_client *client;
	bool ext_range_mode;
	bool read_order;
};

/*
 * Name of the auxiliary device published on the auxiliary bus by the core
 * driver.  The full device name is "amd-sbtsi.temp-sensor.<id>". where
 * <id> is the auxiliary device instance id.
 */
#define AMD_SBTSI_ADEV		"amd-sbtsi"
#define AMD_SBTSI_AUX_HWMON	"temp-sensor"

#endif /* _LINUX_TSI_H_ */
