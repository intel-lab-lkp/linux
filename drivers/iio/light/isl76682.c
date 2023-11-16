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

enum isl76682_als_ir_mode {
	ISL76682_MODE_NONE = 0,
	ISL76682_MODE_ALS,
	ISL76682_MODE_IR,
};

struct isl76682_chip {
	struct mutex			lock;
	struct regmap			*regmap;
	enum isl76682_als_ir_mode	als_ir_mode;
	int				lux_scale;
};

static int isl76682_set_als_scale(struct isl76682_chip *chip, int lux_scale)
{
	int ret, val;

	if (lux_scale == 1000)
		val = ISL76682_COMMAND_RANGE_LUX_1K;
	else if (lux_scale == 4000)
		val = ISL76682_COMMAND_RANGE_LUX_4K;
	else if (lux_scale == 16000)
		val = ISL76682_COMMAND_RANGE_LUX_16K;
	else if (lux_scale == 64000)
		val = ISL76682_COMMAND_RANGE_LUX_64K;
	else
		return -EINVAL;

	ret = regmap_update_bits(chip->regmap, ISL76682_REG_COMMAND,
				 ISL76682_COMMAND_RANGE_LUX_MASK, val);
	if (ret < 0)
		return ret;

	chip->lux_scale = lux_scale;

	return 0;
}

static int isl76682_set_als_ir_mode(struct isl76682_chip *chip,
				    enum isl76682_als_ir_mode mode)
{
	int ret;

	if (chip->als_ir_mode == mode)
		return 0;

	if (mode == ISL76682_MODE_NONE) {
		return regmap_clear_bits(chip->regmap, ISL76682_REG_COMMAND,
					 ISL76682_COMMAND_EN);
	}

	ret = isl76682_set_als_scale(chip, chip->lux_scale);
	if (ret < 0)
		return ret;

	if (mode == ISL76682_MODE_ALS) {
		ret = regmap_clear_bits(chip->regmap, ISL76682_REG_COMMAND,
					ISL76682_COMMAND_LIGHT_IR);
	} else {
		ret = regmap_set_bits(chip->regmap, ISL76682_REG_COMMAND,
				      ISL76682_COMMAND_LIGHT_IR);
	}
	if (ret < 0)
		return ret;

	/* Enable the ALS/IR */
	ret = regmap_set_bits(chip->regmap, ISL76682_REG_COMMAND,
			      ISL76682_COMMAND_EN |
			      ISL76682_COMMAND_MODE_CONTINUOUS);
	if (ret < 0)
		return ret;

	/* Need to wait for conversion time if ALS/IR mode enabled */
	msleep(ISL76682_CONV_TIME_MS);

	chip->als_ir_mode = mode;

	return 0;
}

static int isl76682_read_als_ir(struct isl76682_chip *chip, int *als_ir)
{
	unsigned int lsb, msb;
	int ret;

	ret = regmap_read(chip->regmap, ISL76682_REG_ALSIR_L, &lsb);
	if (ret < 0)
		return ret;

	ret = regmap_read(chip->regmap, ISL76682_REG_ALSIR_U, &msb);
	if (ret < 0)
		return ret;

	*als_ir = (msb << 8) | lsb;

	return 0;
}

static int isl76682_als_get(struct isl76682_chip *chip, int *als_data)
{
	int als_ir_data;
	int ret;

	ret = isl76682_set_als_ir_mode(chip, ISL76682_MODE_ALS);
	if (ret < 0)
		return ret;

	ret = isl76682_read_als_ir(chip, &als_ir_data);
	if (ret < 0)
		return ret;

	*als_data = als_ir_data;

	return 0;
}

static int isl76682_ir_get(struct isl76682_chip *chip, int *ir_data)
{
	int ret;

	ret = isl76682_set_als_ir_mode(chip, ISL76682_MODE_IR);
	if (ret < 0)
		return ret;

	return isl76682_read_als_ir(chip, ir_data);
}

static int isl76682_write_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int val, int val2, long mask)
{
	struct isl76682_chip *chip = iio_priv(indio_dev);
	int ret;

	if (chan->type != IIO_LIGHT)
		return -EINVAL;

	if (mask != IIO_CHAN_INFO_SCALE)
		return -EINVAL;

	mutex_lock(&chip->lock);
	ret = isl76682_set_als_scale(chip, val);
	mutex_unlock(&chip->lock);

	return ret;
}

static int isl76682_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct isl76682_chip *chip = iio_priv(indio_dev);
	int ret;

	mutex_lock(&chip->lock);

	ret = -EINVAL;
	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_LIGHT:
			ret = isl76682_als_get(chip, val);
			break;
		case IIO_INTENSITY:
			ret = isl76682_ir_get(chip, val);
			break;
		default:
			break;
		}

		if (ret < 0)
			break;

		ret = IIO_VAL_INT;
		break;
	case IIO_CHAN_INFO_SCALE:
		if (chan->type != IIO_LIGHT)
			break;
		*val = chip->lux_scale;
		*val2 = ISL76682_ADC_MAX;
		ret = IIO_VAL_FRACTIONAL;
		break;
	default:
		break;
	}

	mutex_unlock(&chip->lock);

	return ret;
}

static IIO_CONST_ATTR(in_illuminance_scale_available, "1000 4000 16000 64000");

#define ISL76682_CONST_ATTR(name) (&iio_const_attr_##name.dev_attr.attr)
static struct attribute *isl76682_attributes[] = {
	ISL76682_CONST_ATTR(in_illuminance_scale_available),
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
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
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
		dev_err(dev, "%s(): Error %d clearing the CONFIGURE register\n",
			__func__, ret);

	chip->als_ir_mode = ISL76682_MODE_NONE;

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
	.reg_bits = 8,
	.val_bits = 8,
	.volatile_reg = isl76682_is_volatile_reg,
	.max_register = ISL76682_NUM_REGS - 1,
	.num_reg_defaults_raw = ISL76682_NUM_REGS,
	.cache_type = REGCACHE_FLAT,
};

static int isl76682_probe(struct i2c_client *client)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(client);
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
		ret = PTR_ERR(chip->regmap);
		dev_err(&client->dev, "%s: Error %d initializing regmap\n",
			__func__, ret);
		return ret;
	}

	chip->lux_scale = 1000;

	ret = isl76682_clear_configure_reg(chip);
	if (ret < 0)
		return ret;

	indio_dev->info = &isl76682_info;
	indio_dev->channels = isl76682_channels;
	indio_dev->num_channels = ARRAY_SIZE(isl76682_channels);
	indio_dev->name = id->name;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = iio_device_register(indio_dev);
	if (ret < 0) {
		dev_err(&client->dev,
			"%s(): iio registration failed with error %d\n",
			__func__, ret);
		return ret;
	}

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
		.name = "isl76682",
		.of_match_table = isl76682_of_match,
	},
	.probe = isl76682_probe,
	.remove = isl76682_remove,
	.id_table = isl76682_id,
};

module_i2c_driver(isl76682_driver);

MODULE_DESCRIPTION("ISL76682 Ambient Light Sensor driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Marek Vasut <marex@denx.de>");
