/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * max77705.h - Driver for the Maxim 77705
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __MAX77705_H__
#define __MAX77705_H__

#define MFD_DEV_NAME "max77705"

struct max77705_platform_data {
	struct power_supply_battery_info *bat_info;
};

#endif /* __MAX77705_H__ */

