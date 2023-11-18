// SPDX-License-Identifier: GPL-2.0-only
/*
 * IIO driver for the light sensor ISL76682.
 * ISL76682 is Ambient Light Sensor
 *
 * Copyright (c) 2023 Marek Vasut <marex@denx.de>
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define ISL76682_REG_COMMAND			0x00

#define ISL76682_COMMAND_EN			BIT(7)
#define ISL76682_COMMAND_MODE_CONTINUOUS	BIT(6)
#define ISL76682_COMMAND_LIGHT_IR		BIT(5)

#define ISL76682_COMMAND_RANGE_LUX_1K		0
#define ISL76682_COMMAND_RANGE_LUX_4K		(1 << 0)
#define ISL76682_COMMAND_RANGE_LUX_16K		(2 << 0)
#define ISL76682_COMMAND_RANGE_LUX_64K		(3 << 0)
#define ISL76682_COMMAND_RANGE_LUX_MASK		GENMASK(1, 0)

#define ISL76682_REG_ALSIR_L			0x01

#define ISL76682_REG_ALSIR_U			0x02

#define ISL76682_NUM_REGS			(ISL76682_REG_ALSIR_U + 1)

#define ISL76682_CONV_TIME_MS			100

#define ISL76682_ADC_MAX			0xffff

struct isl76682_chip {
	struct mutex			lock;
	struct regmap			*regmap;
	u8				range;
	u8				command;
};

static int isl76682_get(struct isl76682_chip *chip, bool mode_ir, int *data)
{
	u8 command;
	int ret;

	command = ISL76682_COMMAND_EN | ISL76682_COMMAND_MODE_CONTINUOUS |
		  chip->range;

	if (mode_ir)
		command |= ISL76682_COMMAND_LIGHT_IR;

	if (command != chip->command) {
		ret = regmap_write(chip->regmap, ISL76682_REG_COMMAND, command);
		if (ret)
			return ret;

		/* Need to wait for conversion time if ALS/IR mode enabled */
		msleep(ISL76682_CONV_TIME_MS);

		chip->command = command;
	}

	return regmap_bulk_read(chip->regmap, ISL76682_REG_ALSIR_L, data, 2);
}

static int isl76682_write_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int val, int val2, long mask)
{
	struct isl76682_chip *chip = iio_priv(indio_dev);
	u8 range;

	if (chan->type != IIO_LIGHT)
		return -EINVAL;

	if (mask != IIO_CHAN_INFO_SCALE)
		return -EINVAL;

	if (val != 0)
		return -EINVAL;

	if (chan->type == IIO_LIGHT) {
		if (val2 == 15000)		/* 0.015 ... 1000 lux */
			range = ISL76682_COMMAND_RANGE_LUX_1K;
		else if (val2 == 60000)		/* 0.060 ... 4000 lux */
			range = ISL76682_COMMAND_RANGE_LUX_4K;
		else if (val2 == 240000)	/* 0.240 ... 16000 lux */
			range = ISL76682_COMMAND_RANGE_LUX_16K;
		else if (val2 == 960000)	/* 0.960 ... 64000 lux */
			range = ISL76682_COMMAND_RANGE_LUX_64K;
		else
			return -EINVAL;
	} else if (chan->type == IIO_INTENSITY) {
		if (val2 == 10500)		/* 0.0105 .. 1000 lux */
			range = ISL76682_COMMAND_RANGE_LUX_1K;
		else if (val2 == 42000)		/* 0.042 ... 4000 lux */
			range = ISL76682_COMMAND_RANGE_LUX_4K;
		else if (val2 == 168000)	/* 0.168 ... 16000 lux */
			range = ISL76682_COMMAND_RANGE_LUX_16K;
		else if (val2 == 673000)	/* 0.673 ... 64000 lux */
			range = ISL76682_COMMAND_RANGE_LUX_64K;
		else
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	mutex_lock(&chip->lock);
	chip->range = range;
	mutex_unlock(&chip->lock);

	return 0;
}

static int isl76682_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct isl76682_chip *chip = iio_priv(indio_dev);
	int ret = -EINVAL;

	mutex_lock(&chip->lock);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_LIGHT:
			ret = isl76682_get(chip, false, val);
			break;
		case IIO_INTENSITY:
			ret = isl76682_get(chip, true, val);
			break;
		default:
			break;
		}

		if (ret < 0)
			break;

		ret = IIO_VAL_INT;
		break;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		switch (chan->type) {
		case IIO_LIGHT:
			if (chip->range == ISL76682_COMMAND_RANGE_LUX_1K)
				*val2 = 15000;
			else if (chip->range == ISL76682_COMMAND_RANGE_LUX_4K)
				*val2 = 60000;
			else if (chip->range == ISL76682_COMMAND_RANGE_LUX_16K)
				*val2 = 240000;
			else if (chip->range == ISL76682_COMMAND_RANGE_LUX_64K)
				*val2 = 960000;
			else
				break;
			ret = IIO_VAL_INT_PLUS_MICRO;
			break;
		case IIO_INTENSITY:
			if (chip->range == ISL76682_COMMAND_RANGE_LUX_1K)
				*val2 = 10500;
			else if (chip->range == ISL76682_COMMAND_RANGE_LUX_4K)
				*val2 = 42000;
			else if (chip->range == ISL76682_COMMAND_RANGE_LUX_16K)
				*val2 = 168000;
			else if (chip->range == ISL76682_COMMAND_RANGE_LUX_64K)
				*val2 = 673000;
			else
				break;
			ret = IIO_VAL_INT_PLUS_MICRO;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	mutex_unlock(&chip->lock);

	return ret;
}

static IIO_CONST_ATTR(in_illuminance_scale_available, "0.015 0.06 0.24 0.96");
static IIO_CONST_ATTR(in_intensity_scale_available, "0.0105 0.042 0.168 0.673");
static IIO_CONST_ATTR(integration_time_available, "0.090");	/* 90 ms */

static struct attribute *isl76682_attributes[] = {
	&iio_const_attr_in_illuminance_scale_available.dev_attr.attr,
	&iio_const_attr_in_intensity_scale_available.dev_attr.attr,
	&iio_const_attr_integration_time_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group isl29108_group = {
	.attrs = isl76682_attributes,
};

static const struct iio_chan_spec isl76682_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	}, {
		.type = IIO_INTENSITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	}
};

static const struct iio_info isl76682_info = {
	.attrs		= &isl29108_group,
	.read_raw	= isl76682_read_raw,
	.write_raw	= isl76682_write_raw,
};

static int isl76682_clear_configure_reg(struct isl76682_chip *chip)
{
	struct device *dev = regmap_get_device(chip->regmap);
	int ret;

	ret = regmap_write(chip->regmap, ISL76682_REG_COMMAND, 0x0);
	if (ret < 0)
		dev_err(dev, "Error %d clearing the CONFIGURE register\n", ret);

	chip->command = 0;

	return ret;
}

static bool isl76682_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ISL76682_REG_ALSIR_L:
	case ISL76682_REG_ALSIR_U:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config isl76682_regmap_config = {
	.reg_bits		= 8,
	.val_bits		= 8,
	.volatile_reg		= isl76682_is_volatile_reg,
	.max_register		= ISL76682_NUM_REGS - 1,
	.num_reg_defaults_raw	= ISL76682_NUM_REGS,
	.cache_type		= REGCACHE_FLAT,
};

static int isl76682_probe(struct i2c_client *client)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(client);
	struct device *dev = &client->dev;
	struct isl76682_chip *chip;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*chip));
	if (!indio_dev)
		return -ENOMEM;

	chip = iio_priv(indio_dev);

	i2c_set_clientdata(client, indio_dev);
	mutex_init(&chip->lock);

	chip->regmap = devm_regmap_init_i2c(client, &isl76682_regmap_config);
	if (IS_ERR(chip->regmap)) {
		return dev_err_probe(dev, PTR_ERR(chip->regmap),
				     "Error initializing regmap\n");
	}

	chip->range = ISL76682_COMMAND_RANGE_LUX_1K;

	ret = isl76682_clear_configure_reg(chip);
	if (ret < 0)
		return ret;

	indio_dev->info = &isl76682_info;
	indio_dev->channels = isl76682_channels;
	indio_dev->num_channels = ARRAY_SIZE(isl76682_channels);
	indio_dev->name = id->name;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = iio_device_register(indio_dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Device registration failed\n");

	return 0;
}

static void isl76682_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct isl76682_chip *chip = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);

	isl76682_clear_configure_reg(chip);
}

static const struct i2c_device_id isl76682_id[] = {
	{"isl76682", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, isl76682_id);

static const struct of_device_id isl76682_of_match[] = {
	{ .compatible = "isil,isl76682", },
	{ },
};
MODULE_DEVICE_TABLE(of, isl76682_of_match);

static struct i2c_driver isl76682_driver = {
	.driver  = {
		.name		= "isl76682",
		.of_match_table	= isl76682_of_match,
	},
	.probe		= isl76682_probe,
	.remove		= isl76682_remove,
	.id_table	= isl76682_id,
};

module_i2c_driver(isl76682_driver);

MODULE_DESCRIPTION("ISL76682 Ambient Light Sensor driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Marek Vasut <marex@denx.de>");
