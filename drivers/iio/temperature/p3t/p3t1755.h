/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * NXP P3T175x Temperature Sensor Driver
 *
 * Copyright 2025 NXP
 */
#ifndef P3T1755_H
#define P3T1755_H

#include <linux/device.h>
#include <linux/iio/iio.h>

#define P3T1755_REG_TEMP		0x0
#define P3T1755_REG_CFGR		0x1
#define P3T1755_REG_LOW_LIM		0x2
#define P3T1755_REG_HIGH_LIM		0x3

#define P3T1755_SHUTDOWN_BIT		BIT(0)
#define P3T1755_TM_BIT			BIT(1)
#define P3T1755_POL_BIT			BIT(2)
#define P3T1755_R0_BIT			BIT(5)
#define P3T1755_R1_BIT			BIT(6)
#define P3T1755_ONE_SHOT_BIT		BIT(7)

#define P3T1755_FAULT_QUEUE_SHIFT	3
#define P3T1755_FAULT_QUEUE_MASK	GENMASK(4, 3)

#define P3T1755_RESOLUTION_10UC		62500

extern const struct p3t17xx_info p3t1755_channels_info;
extern const struct p3t17xx_info p3t1750_channels_info;
extern const struct p3t17xx_info p3t175x_channels_info;

enum p3t1755_hw_id {
	P3T1755_ID = 0,
	P3T1750_ID,
};

struct p3t17xx_info {
	const char *name;
	const struct iio_chan_spec *channels;
	int num_channels;
};

/* Device-specific data structure */
struct p3t1755_data {
	struct device *dev;
	struct regmap *regmap;
	bool tm_mode;
	bool alert_active_high;
};

int p3t1755_fault_queue_to_bits(int val);
int p3t1755_probe(struct device *dev, const struct p3t17xx_info *chip,
		  struct regmap *regmap, bool tm_mode, bool alert_active_high, int fq_bits);
int p3t1755_get_temp_and_limits(struct p3t1755_data *data,
				int *temp_mc, int *thigh_mc, int *tlow_mc);
void p3t1755_push_thresh_event(struct iio_dev *indio_dev);

#endif /* P3T1755_H */
