/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * linux/include/linux/mfd/aat2870.h
 *
 * Copyright (c) 2011, NVIDIA Corporation.
 * Author: Jin Park <jinyoungp@nvidia.com>
 */

#ifndef __LINUX_MFD_AAT2870_H
#define __LINUX_MFD_AAT2870_H

#include <linux/debugfs.h>
#include <linux/i2c.h>

/* Register offsets */
#define AAT2870_BL_CH_EN	0x00
#define AAT2870_BLM		0x01
#define AAT2870_BLS		0x02
#define AAT2870_BL1		0x03
#define AAT2870_BL2		0x04
#define AAT2870_BL3		0x05
#define AAT2870_BL4		0x06
#define AAT2870_BL5		0x07
#define AAT2870_BL6		0x08
#define AAT2870_BL7		0x09
#define AAT2870_BL8		0x0A
#define AAT2870_FLR		0x0B
#define AAT2870_FM		0x0C
#define AAT2870_FS		0x0D
#define AAT2870_ALS_CFG0	0x0E
#define AAT2870_ALS_CFG1	0x0F
#define AAT2870_ALS_CFG2	0x10
#define AAT2870_AMB		0x11
#define AAT2870_ALS0		0x12
#define AAT2870_ALS1		0x13
#define AAT2870_ALS2		0x14
#define AAT2870_ALS3		0x15
#define AAT2870_ALS4		0x16
#define AAT2870_ALS5		0x17
#define AAT2870_ALS6		0x18
#define AAT2870_ALS7		0x19
#define AAT2870_ALS8		0x1A
#define AAT2870_ALS9		0x1B
#define AAT2870_ALSA		0x1C
#define AAT2870_ALSB		0x1D
#define AAT2870_ALSC		0x1E
#define AAT2870_ALSD		0x1F
#define AAT2870_ALSE		0x20
#define AAT2870_ALSF		0x21
#define AAT2870_SUB_SET		0x22
#define AAT2870_SUB_CTRL	0x23
#define AAT2870_LDO_AB		0x24
#define AAT2870_LDO_CD		0x25
#define AAT2870_LDO_EN		0x26
#define AAT2870_REG_NUM		0x27

struct aat2870_data {
	struct device *dev;
	struct i2c_client *client;

	struct mutex io_lock;
	struct aat2870_register *reg_cache; /* register cache */
	struct gpio_desc *en_pin;

	/* i2c io funcntions */
	int (*read)(struct aat2870_data *aat2870, u8 addr, u8 *val);
	int (*write)(struct aat2870_data *aat2870, u8 addr, u8 val);
	int (*update)(struct aat2870_data *aat2870, u8 addr, u8 mask, u8 val);
};

#endif /* __LINUX_MFD_AAT2870_H */
