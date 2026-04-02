/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Realtek Semiconductor Corporation
 * Author: Yu-Chun Lin <eleanor.lin@realtek.com>
 */

#ifndef __RESET_REALTEK_COMMON_H
#define __RESET_REALTEK_COMMON_H

#include <linux/reset-controller.h>

struct regmap;

struct rtk_reset_desc {
	u32 ofs;
	u32 bit;
	bool write_en;
};

struct rtk_reset_data {
	struct reset_controller_dev rcdev;
	struct rtk_reset_desc *descs;
	struct regmap *regmap;
};

int rtk_reset_controller_add(struct device *dev,
			     struct rtk_reset_data *initdata);

#endif /* __RESET_REALTEK_COMMON_H */
