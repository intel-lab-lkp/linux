/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2025 Bharadwaj Raju <bharadwaj.raju777@gmail.com>
 */

#ifndef INV_ICM20948_H_
#define INV_ICM20948_H_

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/err.h>

/* accel takes 20ms, gyro takes 35ms to wake from full-chip sleep */
#define INV_ICM20948_SLEEP_WAKEUP_MS 35

#define INV_ICM20948_REG_BANK_SEL 0x7F
#define INV_ICM20948_BANK_SEL_MASK GENMASK(5, 4)

#define INV_ICM20948_REG_WHOAMI 0x0000
#define INV_ICM20948_WHOAMI 0xEA

#define INV_ICM20948_REG_FIFO_RW 0x0072

#define INV_ICM20948_REG_PWR_MGMT_1 0x0006
#define INV_ICM20948_PWR_MGMT_1_DEV_RESET BIT(7)
#define INV_ICM20948_PWR_MGMT_1_SLEEP BIT(6)

#define INV_ICM20948_REG_TEMP_DATA 0x0039

extern const struct regmap_config inv_icm20948_regmap_config;

struct inv_icm20948_state {
	struct device *dev;
	struct regmap *regmap;
	struct iio_dev *temp_dev;
	struct mutex lock;
};

extern int inv_icm20948_core_probe(struct regmap *regmap);

struct iio_dev *inv_icm20948_temp_init(struct inv_icm20948_state *state);

#endif
