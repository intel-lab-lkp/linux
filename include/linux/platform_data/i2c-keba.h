/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) KEBA AG 2012
 * Copyright (C) KEBA Industrial Automation Gmbh 2024
 *
 * Platform data for KEBA I2C controller FPGA IP core
 */

#ifndef __LINUX_PLATFORM_DATA_I2C_KEBA_H
#define __LINUX_PLATFORM_DATA_I2C_KEBA_H

/**
 * Platform data for KEBA I2C controller
 *
 * @info I2C devices to be probed
 * @info_size size of info array
 */
struct i2c_keba_platform_data {
	struct i2c_board_info *info;
	int info_size;
};

#endif /* __LINUX_PLATFORM_DATA_I2C_KEBA_H */
