/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */
/*
 * Regmap i2c mux driver
 *
 * Copyright (C) 2023 Nvidia Technologies Ltd.
 */

#ifndef __LINUX_PLATFORM_DATA_I2C_MUX_REGMAP_H
#define __LINUX_PLATFORM_DATA_I2C_MUX_REGMAP_H

/**
 * struct i2c_mux_regmap_platform_data - Platform-dependent data for i2c-mux-regmap
 * @regmap: register map of parent device;
 * @parent: Parent I2C bus adapter number
 * @chan_ids - channels array
 * @num_adaps - number of adapters
 * @sel_reg_addr - mux select register offset in CPLD space
 * @reg_size: register size in bytes
 * @handle: handle to be passed by callback
 * @completion_notify: callback to notify when all the adapters are created
 */
struct i2c_mux_regmap_platform_data {
	void *regmap;
	int parent;
	const unsigned int *chan_ids;
	int num_adaps;
	int sel_reg_addr;
	u8 reg_size;
	void *handle;
	int (*completion_notify)(void *handle, struct i2c_adapter *parent,
				 struct i2c_adapter *adapters[]);
};

#endif	/* __LINUX_PLATFORM_DATA_I2C_MUX_REGMAP_H */
