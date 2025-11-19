// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) BECOM Electronics GmbH
 *
 * wsen_tids.c - Linux hwmon driver for WSEN-TIDS Temperature sensor
 *
 * Author: Thomas Marangoni <thomas.marangoni@becom-group.com>
 */

#include <linux/util_macros.h>
#include <linux/regmap.h>
#include <linux/minmax.h>
#include <linux/hwmon.h>
#include <linux/bits.h>
#include <linux/math.h>
#include <linux/i2c.h>

/*
 * TIDS registers
 */
#define TIDS_REG_DEVICE_ID	0x01
#define TIDS_REG_T_H_LIMIT	0x02
#define TIDS_REG_T_L_LIMIT	0x03
#define TIDS_REG_CTRL		0x04
#define TIDS_REG_STATUS		0x05
#define TIDS_REG_DATA_T_L	0x06
#define TIDS_REG_DATA_T_H	0x07
#define TIDS_REG_SOFT_REST	0x0C

#define TIDS_CTRL_ONE_SHOT_MASK		BIT(0)
#define TIDS_CTRL_FREERUN_MASK		BIT(2)
#define TIDS_CTRL_IF_ADD_INC_MASK	BIT(3)
#define TIDS_CTRL_AVG_MASK		GENMASK(5, 4)
#define TIDS_CTRL_AVG_SHIFT		4
#define TIDS_CTRL_BDU_MASK		BIT(6)

#define TIDS_STATUS_BUSY_MASK		BIT(0)
#define TIDS_STATUS_OVER_THL_MASK	BIT(1)
#define TIDS_STATUS_UNDER_TLL_MASK	BIT(2)

#define TIDS_SOFT_REST_MASK		BIT(1)

/*
 * TIDS device IDs
 */
#define TIDS_ID 0xa0

struct tids_data {
	struct i2c_client *client;

	struct regmap *regmap;

	int irq;
	int temperature;
};

static u8 update_intervals[] = { 40, 20, 10, 5 };

static ssize_t tids_interval_read(struct device *dev, long *val)
{
	struct tids_data *data = dev_get_drvdata(dev);
	unsigned int avg_value = 0;
	int ret;

	ret = regmap_read(data->regmap, TIDS_REG_CTRL, &avg_value);
	if (ret < 0)
		return ret;

	avg_value = (avg_value & TIDS_CTRL_AVG_MASK) >> TIDS_CTRL_AVG_SHIFT;

	*val = update_intervals[avg_value];

	return 0;
}

static ssize_t tids_interval_write(struct device *dev, long val)
{
	struct tids_data *data = dev_get_drvdata(dev);
	unsigned int avg_value;

	avg_value = find_closest_descending(val, update_intervals,
					    ARRAY_SIZE(update_intervals));

	return regmap_write_bits(data->regmap, TIDS_REG_CTRL,
				 TIDS_CTRL_AVG_MASK,
				 avg_value << TIDS_CTRL_AVG_SHIFT);
}

static int tids_temperature1_read(struct device *dev, long *val)
{
	struct tids_data *data = dev_get_drvdata(dev);
	u8 buf[2] = { 0 };
	int ret;

	ret = regmap_bulk_read(data->regmap, TIDS_REG_DATA_T_L, buf, 2);
	if (ret < 0)
		return ret;

	/* temperature in °mC */
	*val = (((s16)(buf[1] << 8) | buf[0])) * 10;

	return 0;
}

static ssize_t tids_temperature_alarm_read(struct device *dev, u32 attr,
					   long *val)
{
	struct tids_data *data = dev_get_drvdata(dev);
	int ret;

	if (attr == hwmon_temp_min_alarm)
		ret = regmap_test_bits(data->regmap, TIDS_REG_STATUS,
				       TIDS_STATUS_UNDER_TLL_MASK);
	else if (attr == hwmon_temp_max_alarm)
		ret = regmap_test_bits(data->regmap, TIDS_REG_STATUS,
				       TIDS_STATUS_OVER_THL_MASK);
	else
		return -EOPNOTSUPP;

	if (ret < 0)
		return ret;

	*val = ret;

	return 0;
}

static int tids_temperature_minmax_read(struct device *dev, u32 attr, long *val)
{
	struct tids_data *data = dev_get_drvdata(dev);
	unsigned int reg_data = 0;
	int ret;

	if (attr == hwmon_temp_min)
		ret = regmap_read(data->regmap, TIDS_REG_T_L_LIMIT, &reg_data);
	else if (attr == hwmon_temp_max)
		ret = regmap_read(data->regmap, TIDS_REG_T_H_LIMIT, &reg_data);
	else
		return -EOPNOTSUPP;

	if (ret < 0)
		return ret;

	/* temperature from register conversion in °mC */
	*val = (((u8)reg_data - 63) * 640);

	return 0;
}

static ssize_t tids_temperature_minmax_write(struct device *dev, u32 attr,
					     long val)
{
	struct tids_data *data = dev_get_drvdata(dev);
	u8 reg_data;

	/* temperature in °mC */
	val = clamp_val(val, -39680, 122880);
	/* temperature to register conversion in °mC */
	reg_data = (u8)(DIV_ROUND_CLOSEST(val, 640) + 63);

	if (attr == hwmon_temp_min)
		return regmap_write(data->regmap, TIDS_REG_T_L_LIMIT, reg_data);
	else if (attr == hwmon_temp_max)
		return regmap_write(data->regmap, TIDS_REG_T_H_LIMIT, reg_data);
	else
		return -EOPNOTSUPP;
}

static umode_t tids_hwmon_visible(const void *data,
				  enum hwmon_sensor_types type, u32 attr,
				  int channel)
{
	umode_t mode = 0;

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
		case hwmon_temp_min_alarm:
		case hwmon_temp_max_alarm:
			mode = 0444;
			break;
		case hwmon_temp_min:
		case hwmon_temp_max:
			mode = 0644;
			break;
		default:
			break;
		}
		break;
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			mode = 0644;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return mode;
}

static int tids_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long *val)
{
	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			return tids_temperature1_read(dev, val);
		case hwmon_temp_min_alarm:
		case hwmon_temp_max_alarm:
			return tids_temperature_alarm_read(dev, attr, val);
		case hwmon_temp_min:
		case hwmon_temp_max:
			return tids_temperature_minmax_read(dev, attr, val);
		default:
			return -EOPNOTSUPP;
		}
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			return tids_interval_read(dev, val);
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static int tids_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long val)
{
	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_min:
		case hwmon_temp_max:
			return tids_temperature_minmax_write(dev, attr, val);
		default:
			return -EOPNOTSUPP;
		}
	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			return tids_interval_write(dev, val);
		default:
			return -EOPNOTSUPP;
		}
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info *const tids_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_UPDATE_INTERVAL),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_MIN_ALARM |
			  HWMON_T_MAX_ALARM | HWMON_T_MIN | HWMON_T_MAX),
	NULL
};

static const struct hwmon_ops tids_ops = {
	.is_visible	= tids_hwmon_visible,
	.read		= tids_hwmon_read,
	.write		= tids_hwmon_write,
};

static const struct hwmon_chip_info tids_chip_info = {
	.ops	= &tids_ops,
	.info	= tids_info,
};

static bool tids_regmap_writeable_reg(struct device *dev, unsigned int reg)
{
	if (reg >= TIDS_REG_T_H_LIMIT && reg <= TIDS_REG_CTRL)
		return true;

	if (reg == TIDS_REG_SOFT_REST)
		return true;

	return false;
}

static bool tids_regmap_readable_reg(struct device *dev, unsigned int reg)
{
	if (reg >= TIDS_REG_DEVICE_ID && reg <= TIDS_REG_DATA_T_H)
		return true;

	if (reg == TIDS_REG_SOFT_REST)
		return true;

	return false;
}

static bool tids_regmap_volatile_reg(struct device *dev, unsigned int reg)
{
	if (reg >= TIDS_REG_STATUS && reg <= TIDS_REG_DATA_T_H)
		return true;

	if (reg == TIDS_REG_SOFT_REST)
		return true;

	return false;
}

static const struct regmap_config regmap_config = {
	.reg_bits		= 8,
	.val_bits		= 8,
	.max_register		= TIDS_REG_SOFT_REST,
	.writeable_reg		= tids_regmap_writeable_reg,
	.readable_reg		= tids_regmap_readable_reg,
	.volatile_reg		= tids_regmap_volatile_reg,
	.use_single_read	= 0,
};

static int tids_init(struct tids_data *data)
{
	int ret;

	/* Triggering soft reset */
	ret = regmap_write_bits(data->regmap, TIDS_REG_SOFT_REST,
				TIDS_SOFT_REST_MASK, TIDS_SOFT_REST_MASK);
	if (ret < 0)
		return ret;

	ret = regmap_clear_bits(data->regmap, TIDS_REG_SOFT_REST,
				TIDS_SOFT_REST_MASK);
	if (ret < 0)
		return ret;

	/* Allowing bulk read */
	ret = regmap_write_bits(data->regmap, TIDS_REG_CTRL,
				TIDS_CTRL_IF_ADD_INC_MASK,
				TIDS_CTRL_IF_ADD_INC_MASK);
	if (ret < 0)
		return ret;

	/* Set meassurement interval */
	ret = regmap_clear_bits(data->regmap, TIDS_REG_CTRL,
				TIDS_CTRL_AVG_MASK);
	if (ret < 0)
		return ret;

	/* Set device to free run mode */
	ret = regmap_write_bits(data->regmap, TIDS_REG_CTRL,
				TIDS_CTRL_FREERUN_MASK, TIDS_CTRL_FREERUN_MASK);
	if (ret < 0)
		return ret;

	/* Don't update temperature register until high and low value are read */
	ret = regmap_write_bits(data->regmap, TIDS_REG_CTRL, TIDS_CTRL_BDU_MASK,
				TIDS_CTRL_BDU_MASK);
	if (ret < 0)
		return ret;

	return 0;
}

static int tids_probe(struct i2c_client *client)
{
	struct device *device = &client->dev;
	struct device *hwmon_dev;
	struct tids_data *data;
	unsigned int value;
	int ret;

	data = devm_kzalloc(device, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;

	/* Init regmap */
	data->regmap = devm_regmap_init_i2c(data->client, &regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(device, PTR_ERR(data->regmap),
				     "regmap initialization failed\n");

	/* Read device id, to check if i2c is working */
	ret = regmap_read(data->regmap, TIDS_REG_DEVICE_ID, &value);
	if (ret < 0)
		return ret;

	if (value != TIDS_ID)
		return -ENODEV;

	tids_init(data);

	hwmon_dev = devm_hwmon_device_register_with_info(device, "tids", data,
							 &tids_chip_info, NULL);

	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static int tids_suspend(struct device *dev)
{
	struct tids_data *data = dev_get_drvdata(dev);

	return regmap_clear_bits(data->regmap, TIDS_REG_CTRL,
				 TIDS_CTRL_FREERUN_MASK);
}

static int tids_resume(struct device *dev)
{
	struct tids_data *data = dev_get_drvdata(dev);

	return regmap_write_bits(data->regmap, TIDS_REG_CTRL,
				 TIDS_CTRL_FREERUN_MASK,
				 TIDS_CTRL_FREERUN_MASK);
}

static DEFINE_SIMPLE_DEV_PM_OPS(tids_dev_pm_ops, tids_resume, tids_suspend);

static const struct i2c_device_id tids_id[] = {
	{ "tids", 0 },
	{},
};
MODULE_DEVICE_TABLE(i2c, tids_id);

static const struct of_device_id tids_of_match[] = {
	{ .compatible = "wsen,tids" },
	{}
};
MODULE_DEVICE_TABLE(of, tids_of_match);

static struct i2c_driver tids_driver = {
	.class	  = I2C_CLASS_HWMON,
	.driver   = {
		.name		= "tids",
		.pm		= pm_sleep_ptr(&tids_dev_pm_ops),
		.of_match_table	= tids_of_match,
	},
	.probe    = tids_probe,
	.id_table = tids_id,
};

module_i2c_driver(tids_driver);

MODULE_AUTHOR("Thomas Marangoni <Thomas.Marangoni@becom-group.com>");
MODULE_DESCRIPTION("WSEN TIDS temperature sensor driver");
MODULE_LICENSE("GPL");
