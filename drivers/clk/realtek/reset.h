/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2019 Realtek Semiconductor Corporation
 * Author: Cheng-Yu Lee <cylee12@realtek.com>
 */

#ifndef __CLK_REALTEK_RESET_H
#define __CLK_REALTEK_RESET_H

#include <linux/regmap.h>
#include <linux/reset-controller.h>
#include <linux/hwspinlock.h>

struct rtk_reset_bank {
	u32 ofs;
	u32 write_en;
};

struct rtk_reset_data {
	struct device *dev;
	struct reset_controller_dev rcdev;
	struct rtk_reset_bank *banks;
	u32 num_banks;
	struct regmap *regmap;
};

struct rtk_reset_initdata {
	struct rtk_reset_bank *banks;
	u32 num_banks;
	struct regmap *regmap;
};

int rtk_reset_controller_add(struct device *dev,
			     struct rtk_reset_initdata *initdata);

#endif /* __CLK_REALTEK_RESET_H */
