/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */

#ifndef BMI270_H_
#define BMI270_H_

#include <linux/iio/iio.h>

#define BMI270_CHIP_ID_REG				0x00
#define BMI270_CHIP_ID_VAL				0x24
#define BMI270_CHIP_ID_MSK				GENMASK(7, 0)

#define BMI270_ACCEL_X_REG				0x0c
#define BMI270_ANG_VEL_X_REG				0x12

#define BMI270_INTERNAL_STATUS_REG			0x21
#define BMI270_INTERNAL_STATUS_MSG_MSK			GENMASK(3, 0)
#define BMI270_INTERNAL_STATUS_MSG_INIT_OK		0x01

#define BMI270_INTERNAL_STATUS_AXES_REMAP_ERR_MSK	BIT(5)
#define BMI270_INTERNAL_STATUS_ODR_50HZ_ERR_MSK		BIT(6)

#define BMI270_ACC_CONF_REG				0x40
#define BMI270_ACC_CONF_ODR_MSK				GENMASK(3, 0)
#define BMI270_ACC_CONF_ODR_100HZ			0x08
#define BMI270_ACC_CONF_BWP_MSK				GENMASK(6, 4)
#define BMI270_ACC_CONF_BWP_NORMAL_MODE			0x02
#define BMI270_ACC_CONF_FILTER_PERF_MSK			BIT(7)

#define BMI270_GYR_CONF_REG				0x42
#define BMI270_GYR_CONF_ODR_MSK				GENMASK(3, 0)
#define BMI270_GYR_CONF_ODR_200HZ			0x09
#define BMI270_GYR_CONF_BWP_MSK				GENMASK(5, 4)
#define BMI270_GYR_CONF_BWP_NORMAL_MODE			0x02
#define BMI270_GYR_CONF_NOISE_PERF_MSK			BIT(6)
#define BMI270_GYR_CONF_FILTER_PERF_MSK			BIT(7)

#define BMI270_INIT_CTRL_REG				0x59
#define BMI270_INIT_CTRL_LOAD_DONE_MSK			BIT(0)

#define BMI270_INIT_DATA_REG				0x5e

#define BMI270_PWR_CONF_REG				0x7c
#define BMI270_PWR_CONF_ADV_PWR_SAVE_MSK		BIT(0)
#define BMI270_PWR_CONF_FIFO_WKUP_MSK			BIT(1)
#define BMI270_PWR_CONF_FUP_EN_MSK			BIT(2)

#define BMI270_PWR_CTRL_REG				0x7d
#define BMI270_PWR_CTRL_AUX_EN_MSK			BIT(0)
#define BMI270_PWR_CTRL_GYR_EN_MSK			BIT(1)
#define BMI270_PWR_CTRL_ACCEL_EN_MSK			BIT(2)
#define BMI270_PWR_CTRL_TEMP_EN_MSK			BIT(3)

struct bmi270_data {
	struct device *dev;
	struct regmap *regmap;
};

extern const struct regmap_config bmi270_regmap_config;

int bmi270_core_probe(struct device *dev, struct regmap *regmap);

#endif  /* BMI270_H_ */
