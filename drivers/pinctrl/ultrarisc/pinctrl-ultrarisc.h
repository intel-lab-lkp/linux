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

#define UR_FUNC_DEFAULT		0U
#define UR_FUNC_0		1U
#define UR_FUNC_1		0x10000U

#define UR_MAX_PINS_PER_PORT	16

#define UR_BIAS_MASK		0x0000000F
#define UR_PULL_MASK		0x0C
#define UR_PULL_DIS		0
#define UR_PULL_UP		1
#define UR_PULL_DOWN		2
#define UR_DRIVE_MASK		0x03

struct ur_port_desc {
	u32 pin_base;
	u32 npins;
	u32 func_offset;
	u32 conf_offset;
	u32 supported_modes;
	bool supports_gpio;
};

struct ur_function_desc {
	const char *name;
	u32 mode;
	bool gpio;
	u64 valid_pins;
};

struct ur_pin_val {
	const struct ur_port_desc *port_desc;
	u32 pin;
	union {
		u32 mode;
		u32 conf;
	};
};

struct ur_pinctrl_data {
	const struct pinctrl_pin_desc *pins;
	u32 npins;
	const struct ur_function_desc *functions;
	u32 num_functions;
};

struct ur_pinctrl {
	struct device *dev;
	struct pinctrl_dev *pctl_dev;
	void __iomem *base;
	const struct ur_pinctrl_data *data;
	raw_spinlock_t lock;
	const char **group_names;
	unsigned int *group_pins;
};

int ur_pinctrl_probe(struct platform_device *pdev,
		     const struct ur_pinctrl_data *data);

#endif
