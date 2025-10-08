/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * NXP P3T175x Temperature Sensor Driver
 *
 * Copyright 2025 NXP
 */
#ifndef P3T1755_H
#define P3T1755_H

#include <linux/bits.h>
#include <linux/iio/iio.h>

#define P3T1755_REG_TEMP	0x0
#define P3T1755_REG_CFGR	0x1
#define P3T1755_REG_LOW_LIM	0x2
#define P3T1755_REG_HIGH_LIM	0x3

#define P3T1755_SHUTDOWN_BIT	BIT(0)
#define P3T1755_TM_BIT	BIT(1)

#define P3T1755_CONVERSION_TIME_BITS	GENMASK(6, 5)

#define MICRO 1000000

struct p3t1755_info {
	const char *name;
	const struct iio_chan_spec *channels;
	int num_channels;
};

struct p3t1755_data {
	struct device *dev;
	struct regmap *regmap;
};

extern const struct p3t1755_info p3t1750_channels_info;
extern const struct p3t1755_info p3t1755_channels_info;

int p3t1755_probe(struct device *dev, const struct p3t1755_info *chip,
		  struct regmap *regmap, int irq);
int p3t1755_get_temp_and_limits(struct p3t1755_data *data,
				int *temp_raw, int *thigh_raw, int *tlow_raw);
void p3t1755_push_thresh_event(struct iio_dev *indio_dev);

#endif /* P3T1755_H */
