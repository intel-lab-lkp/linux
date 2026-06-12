/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Texas Instruments ADS1262 ADC driver
 *
 * Copyright (C) 2025 Kurt Borja <kuurtb@gmail.com>
 */

#ifndef _ADS1262_H_
#define _ADS1262_H_

#include <linux/auxiliary_bus.h>
#include <linux/types.h>

struct ads1263_adc2_channel {
	/* ADC2CFG */
	u8 gain:3;
	u8 refmux:3;
	u8 data_rate:2;

	/* ADC2MUX */
	u8 negative_input:4;
	u8 positive_input:4;
};

struct ads1263_adc2_ctx {
	struct auxiliary_device adev;
	struct ads1262 *chip;
	/* Protects channel state */
	struct mutex chan_lock;
	struct ads1263_adc2_channel *channels;
	unsigned int num_channels;
	int (*enable)(struct ads1263_adc2_ctx *ctx,
		      const struct ads1263_adc2_channel *chan);
	int (*start)(struct ads1263_adc2_ctx *ctx);
	int (*stop)(struct ads1263_adc2_ctx *ctx);
	int (*read)(struct ads1263_adc2_ctx *ctx, __be32 *val);
};

#endif
