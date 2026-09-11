/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * i2c-cgbc interface to platform code
 *
 * Copyright (C) 2026 congatec GmbH
 * Author: Thomas Richard <thomas.richard@bootlin.com>
 */

#ifndef _LINUX_I2C_CGBC_H
#define _LINUX_I2C_CGBC_H

/**
 * struct cgbc_platform_data - Platform data of the CGBC I2C driver
 * @name:		I2C adapter name
 * @cgbc_bus_id:	I2C bus ID (from Board Controller point of view)
 * @fixed_freq:		I2C bus has a fixed frequency
 * @nb_devices:		number of devices to add when the driver is probed
 * @devices:		devices to add
 */
struct cgbc_i2c_platform_data {
	const char *name;
	int cgbc_bus_id;
	bool fixed_freq;
	int nb_devices;
	struct i2c_board_info const *devices;
};

#endif /* _LINUX_I2C_CGBC_H */
