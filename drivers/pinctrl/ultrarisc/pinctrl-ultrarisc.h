/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 UltraRISC Technology (Shanghai) Co., Ltd.
 *
 * Author: Jia Wang <wangjia@ultrarisc.com>
 */

#ifndef __PINCTRL_ULTRARISC_H__
#define __PINCTRL_ULTRARISC_H__

#include <linux/io.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/spinlock.h>

struct platform_device;

struct ur_pin_val {
	u32 port;
	u32 pin;
	union {
		u32 mode;
		u32 conf;
	};
#define UR_FUNC_DEF		0
#define UR_FUNC0		1
#define UR_FUNC1		0x10000

#define UR_BIAS_MASK		0x0000000F
#define UR_PULL_MASK		0x0C
#define UR_PULL_DIS		0
#define UR_PULL_UP		1
#define UR_PULL_DOWN		2
#define UR_DRIVE_MASK		0x03
};

struct ur_port_desc {
	const char *name;
	u32 npins;
	u32 func_offset;
	u32 conf_offset;
	u32 supported_modes;
};

struct ur_function_desc {
	const char *name;
	u32 mode;
	bool gpio;
};

struct ur_pinctrl_match_data {
	const struct pinctrl_pin_desc *pins;
	u32 npins;
	const struct ur_function_desc *functions;
	u32 num_functions;
	u32 num_ports;
	struct ur_port_desc ports[];
};

struct ur_pinctrl {
	struct device *dev;
	struct pinctrl_dev *pctl_dev;
	void __iomem *base;
	const struct ur_pinctrl_match_data *match_data;
	raw_spinlock_t lock;
	const char **group_names;
	unsigned int *group_pins;
};

int ur_pinctrl_probe(struct platform_device *pdev);

#endif
