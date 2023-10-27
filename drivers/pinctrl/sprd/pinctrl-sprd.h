/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Driver header file for pin controller driver
 * Copyright (C) 2017 Spreadtrum  - http://www.spreadtrum.com
 */

#ifndef __PINCTRL_SPRD_H__
#define __PINCTRL_SPRD_H__

#include <linux/bits.h>

struct platform_device;

#define NUM_OFFSET	22
#define TYPE_OFFSET	18
#define BIT_OFFSET	10
#define WIDTH_OFFSET	6

#define NUM_MASK	GENMASK(10, 0)
#define TYPE_MASK	GENMASK(3, 0)
#define BIT_MASK	GENMASK(7, 0)
#define WIDTH_MASK	GENMASK(3, 0)
#define REG_MASK	GENMASK(5, 0)

#define SPRD_PIN_INFO(num, type, offset, width, reg)		\
		(((num) & NUM_MASK) << NUM_OFFSET |		\
		 ((type) & TYPE_MASK) << TYPE_OFFSET |		\
		 ((offset) & BIT_MASK) << BIT_OFFSET |		\
		 ((width) & WIDTH_MASK) << WIDTH_OFFSET |	\
		 ((reg) & REG_MASK))

#define SPRD_PINCTRL_PIN(pin)	SPRD_PINCTRL_PIN_DATA(pin, #pin)

#define SPRD_PINCTRL_PIN_DATA(a, b)					\
	{								\
		.name = b,						\
		.num = (((a) >> NUM_OFFSET) & NUM_MASK),		\
		.type = (((a) >> TYPE_OFFSET) & TYPE_MASK),		\
		.bit_offset = (((a) & BIT_OFFSET) & BIT_MASK),		\
		.bit_width = (((a) & WIDTH_OFFSET) & WIDTH_MASK),	\
		.reg = ((a) & REG_MASK)					\
	}

enum pin_type {
	GLOBAL_CTRL_PIN,
	COMMON_PIN,
	MISC_PIN,
};

struct sprd_pins_info {
	const char *name;
	unsigned int num;
	enum pin_type type;

	/* for global control pins configuration */
	unsigned long bit_offset;
	unsigned long bit_width;
	unsigned int reg;
};

struct sprd_pinctrl_priv_data {
	unsigned long common_offset;
	unsigned long misc_offset;
};

int sprd_pinctrl_core_probe(struct platform_device *pdev,
			    struct sprd_pins_info *sprd_soc_pin_info,
			    int pins_cnt);
void sprd_pinctrl_remove(struct platform_device *pdev);
void sprd_pinctrl_shutdown(struct platform_device *pdev);

#endif /* __PINCTRL_SPRD_H__ */
