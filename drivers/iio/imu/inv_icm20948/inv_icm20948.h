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
 #include <linux/pm_runtime.h>

/* accel takes 20ms, gyro takes 35ms to wake from full-chip sleep */
 #define INV_ICM20948_SLEEP_WAKEUP_MS 35

 #define INV_ICM20948_SUSPEND_DELAY_MS 2000

 #define INV_ICM20948_REG_BANK_SEL 0x7F
 #define INV_ICM20948_BANK_SEL_MASK GENMASK(5, 4)

 #define INV_ICM20948_REG_WHOAMI 0x0000
 #define INV_ICM20948_WHOAMI 0xEA

 #define INV_ICM20948_REG_FIFO_RW 0x0072

 #define INV_ICM20948_REG_PWR_MGMT_1 0x0006
 #define INV_ICM20948_PWR_MGMT_1_DEV_RESET BIT(7)
 #define INV_ICM20948_PWR_MGMT_1_SLEEP BIT(6)

 #define INV_ICM20948_REG_TEMP_DATA 0x0039

 #define INV_ICM20948_REG_GYRO_DATA_X 0x0033
 #define INV_ICM20948_REG_GYRO_DATA_Y 0x0035
 #define INV_ICM20948_REG_GYRO_DATA_Z 0x0037

 #define INV_ICM20948_REG_GYRO_CONFIG_1 0x2001
 #define INV_ICM20948_GYRO_CONFIG_ENABLE_DLPF BIT(0)
 #define INV_ICM20948_GYRO_CONFIG_FULLSCALE GENMASK(2, 1)
 #define INV_ICM20948_GYRO_CONFIG_DLP_CONFIG GENMASK(5, 3)

 #define INV_ICM20948_REG_GYRO_USER_OFFSET_X 0x2003
 #define INV_ICM20948_REG_GYRO_USER_OFFSET_Y 0x2005
 #define INV_ICM20948_REG_GYRO_USER_OFFSET_Z 0x2007

extern const struct regmap_config inv_icm20948_regmap_config;

extern const struct dev_pm_ops inv_icm20948_pm_ops;

enum inv_icm20948_gyro_fs {
	INV_ICM20948_GYRO_FS_250 = 0,
	INV_ICM20948_GYRO_FS_500 = 1,
	INV_ICM20948_GYRO_FS_1000 = 2,
	INV_ICM20948_GYRO_FS_2000 = 3,
};

enum inv_icm20948_gyro_avg {
	INV_ICM20948_GYRO_AVG_1X = 0,
	INV_ICM20948_GYRO_AVG_2X = 1,
	INV_ICM20948_GYRO_AVG_4X = 2,
	INV_ICM20948_GYRO_AVG_8X = 3,
	INV_ICM20948_GYRO_AVG_16X = 4,
	INV_ICM20948_GYRO_AVG_32X = 5,
	INV_ICM20948_GYRO_AVG_64X = 6,
	INV_ICM20948_GYRO_AVG_128X = 7,
};

struct inv_icm20948_gyro_config {
	int fsr;
};

struct inv_icm20948_state {
	struct device *dev;
	struct regmap *regmap;
	struct iio_dev *temp_dev;
	struct iio_dev *gyro_dev;
	struct inv_icm20948_gyro_config *gyro_conf;
	struct mutex lock;
};

extern int inv_icm20948_core_probe(struct regmap *regmap);

struct iio_dev *inv_icm20948_temp_init(struct inv_icm20948_state *state);
struct iio_dev *inv_icm20948_gyro_init(struct inv_icm20948_state *state);

int inv_icm20948_pm_setup(struct inv_icm20948_state *state);

 #endif
