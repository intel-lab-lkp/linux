/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 Ambarella, Inc.
 */

#ifndef __CCU_COMMON_H
#define __CCU_COMMON_H

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/regmap.h>

#define AMB_RCT_REG_SIZE	0x1000

struct amb_ccu {
	struct device *dev;
	struct regmap *map;
	struct clk_hw_onecell_data *data;
};

struct amb_ccu *amb_ccu_init(struct platform_device *pdev,
			     unsigned int num_clks);
int amb_ccu_register(struct amb_ccu *ccu);

#endif
