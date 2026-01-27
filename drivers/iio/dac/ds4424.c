// SPDX-License-Identifier: GPL-2.0-only
/*
 * Maxim Integrated
 * 7-bit, Multi-Channel Sink/Source Current DAC Driver
 * Copyright (C) 2017 Maxim Integrated
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/iio/consumer.h>
#include <linux/iio/driver.h>
#include <linux/iio/iio.h>
#include <linux/iio/machine.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define DS4422_MAX_DAC_CHANNELS		2
#define DS4424_MAX_DAC_CHANNELS		4

#define DS4424_DAC_ADDR(chan)   ((chan) + 0xf8)
#define DS4424_SOURCE_I		1
#define DS4424_SINK_I		0

#define DS4424_CHANNEL(chan) { \
	.type = IIO_CURRENT, \
	.indexed = 1, \
	.output = 1, \
	.channel = chan, \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW), \
}

/*
 * DS4424 DAC control register 8 bits
 * [7]		0: to sink; 1: to source
 * [6:0]	steps to sink/source
 * bit[7] looks like a sign bit, but the value of the register is
 * not a two's complement code considering the bit[6:0] is a absolute
 * distance from the zero point.
 */
union ds4424_raw_data {
	struct {
		u8 dx:7;
		u8 source_bit:1;
	};
	u8 bits;
};

enum ds4424_device_ids {
	ID_DS4402,
	ID_DS4404,
	ID_DS4422,
	ID_DS4424,
};

struct ds4424_data {
	struct regmap *regmap;
	struct regulator *vcc_reg;
};

static const struct iio_chan_spec ds4424_channels[] = {
	DS4424_CHANNEL(0),
	DS4424_CHANNEL(1),
	DS4424_CHANNEL(2),
	DS4424_CHANNEL(3),
};

static const struct regmap_range ds44x2_ranges[] = {
	regmap_reg_range(DS4424_DAC_ADDR(0), DS4424_DAC_ADDR(1)),
};

static const struct regmap_range ds44x4_ranges[] = {
	regmap_reg_range(DS4424_DAC_ADDR(0), DS4424_DAC_ADDR(3)),
};

static const struct regmap_access_table ds44x2_table = {
	.yes_ranges = ds44x2_ranges,
	.n_yes_ranges = ARRAY_SIZE(ds44x2_ranges),
};

static const struct regmap_access_table ds44x4_table = {
	.yes_ranges = ds44x4_ranges,
	.n_yes_ranges = ARRAY_SIZE(ds44x4_ranges),
};

static const struct regmap_config ds44x2_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_FLAT,
	.max_register = DS4424_DAC_ADDR(1),
	.rd_table = &ds44x2_table,
	.wr_table = &ds44x2_table,
};

static const struct regmap_config ds44x4_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_FLAT,
	.max_register = DS4424_DAC_ADDR(3),
	.rd_table = &ds44x4_table,
	.wr_table = &ds44x4_table,
};

static int ds4424_init_regmap(struct i2c_client *client,
			      struct iio_dev *indio_dev)
{
	struct ds4424_data *data = iio_priv(indio_dev);
	const struct regmap_config *regmap_config;

	if (indio_dev->num_channels == DS4424_MAX_DAC_CHANNELS)
		regmap_config = &ds44x4_regmap_config;
	else
		regmap_config = &ds44x2_regmap_config;

	data->regmap = devm_regmap_init_i2c(client, regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(data->regmap),
				     "Failed to init regmap.\n");

	return 0;
}

static int ds4424_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct ds4424_data *data = iio_priv(indio_dev);
	union ds4424_raw_data raw;
	unsigned int regval;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_read(data->regmap, DS4424_DAC_ADDR(chan->channel),
				  &regval);
		if (ret < 0) {
			pr_err("%s : regmap_read returned %d\n",
						__func__, ret);
			return ret;
		}
		raw.bits = regval;
		*val = raw.dx;
		if (raw.source_bit == DS4424_SINK_I)
			*val = -*val;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int ds4424_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct ds4424_data *data = iio_priv(indio_dev);
	union ds4424_raw_data raw;

	if (val2 != 0)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (val < S8_MIN || val > S8_MAX)
			return -EINVAL;

		if (val > 0) {
			raw.source_bit = DS4424_SOURCE_I;
			raw.dx = val;
		} else {
			raw.source_bit = DS4424_SINK_I;
			raw.dx = -val;
		}

		return regmap_write(data->regmap, DS4424_DAC_ADDR(chan->channel),
				    raw.bits);

	default:
		return -EINVAL;
	}
}

static int ds4424_verify_chip(struct iio_dev *indio_dev)
{
	struct ds4424_data *data = iio_priv(indio_dev);
	u8 raw_values[DS4424_MAX_DAC_CHANNELS];
	int ret;

	/* Bulk read all channels starting at 0xf8.
	 * This populates the regmap cache with current HW values.
	 */
	ret = regmap_bulk_read(data->regmap, DS4424_DAC_ADDR(0),
			       raw_values, indio_dev->num_channels);
	if (ret)
		return dev_err_probe(&indio_dev->dev, ret, "Failed to seed cache\n");

	return 0;
}

static int ds4424_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ds4424_data *data = iio_priv(indio_dev);
	int ret;

	/* Disable all outputs, bypass cache so the '0' isn't saved */
	regcache_cache_bypass(data->regmap, true);
	for (unsigned int i = 0; i < indio_dev->num_channels; i++) {
		ret = regmap_write(data->regmap, DS4424_DAC_ADDR(i), 0);
		if (ret) {
			dev_err(dev, "Failed to zero channel %d: %d\n", i, ret);
			regcache_cache_bypass(data->regmap, false);
			return ret;
		}
	}
	regcache_cache_bypass(data->regmap, false);

	regcache_cache_only(data->regmap, true);
	regcache_mark_dirty(data->regmap);

	return 0;
}

static int ds4424_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ds4424_data *data = iio_priv(indio_dev);

	regcache_cache_only(data->regmap, false);
	return regcache_sync(data->regmap);
}

static DEFINE_SIMPLE_DEV_PM_OPS(ds4424_pm_ops, ds4424_suspend, ds4424_resume);

static const struct iio_info ds4424_info = {
	.read_raw = ds4424_read_raw,
	.write_raw = ds4424_write_raw,
};

static int ds4424_probe(struct i2c_client *client)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(client);
	struct ds4424_data *data;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);
	indio_dev->name = id->name;

	data->vcc_reg = devm_regulator_get(&client->dev, "vcc");
	if (IS_ERR(data->vcc_reg))
		return dev_err_probe(&client->dev, PTR_ERR(data->vcc_reg),
				     "Failed to get vcc-supply regulator.\n");

	ret = regulator_enable(data->vcc_reg);
	if (ret < 0) {
		dev_err(&client->dev,
				"Unable to enable the regulator.\n");
		return ret;
	}

	usleep_range(1000, 1200);

	switch (id->driver_data) {
	case ID_DS4402:
		indio_dev->num_channels = DS4422_MAX_DAC_CHANNELS;
		break;
	case ID_DS4404:
		indio_dev->num_channels = DS4424_MAX_DAC_CHANNELS;
		break;
	case ID_DS4422:
		indio_dev->num_channels = DS4422_MAX_DAC_CHANNELS;
		break;
	case ID_DS4424:
		indio_dev->num_channels = DS4424_MAX_DAC_CHANNELS;
		break;
	default:
		dev_err(&client->dev,
				"ds4424: Invalid chip id.\n");
		ret = -ENXIO;
		goto fail;
	}

	ret = ds4424_init_regmap(client, indio_dev);
	if (ret < 0)
		goto fail;

	ret = ds4424_verify_chip(indio_dev);
	if (ret < 0)
		goto fail;

	indio_dev->channels = ds4424_channels;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ds4424_info;

	ret = iio_device_register(indio_dev);
	if (ret < 0) {
		dev_err(&client->dev,
				"iio_device_register failed. ret: %d\n", ret);
		goto fail;
	}

	return ret;

fail:
	regulator_disable(data->vcc_reg);
	return ret;
}

static void ds4424_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ds4424_data *data = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	regulator_disable(data->vcc_reg);
}

static const struct i2c_device_id ds4424_id[] = {
	{ "ds4402", ID_DS4402 },
	{ "ds4404", ID_DS4404 },
	{ "ds4422", ID_DS4422 },
	{ "ds4424", ID_DS4424 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, ds4424_id);

static const struct of_device_id ds4424_of_match[] = {
	{ .compatible = "maxim,ds4402" },
	{ .compatible = "maxim,ds4404" },
	{ .compatible = "maxim,ds4422" },
	{ .compatible = "maxim,ds4424" },
	{ }
};

MODULE_DEVICE_TABLE(of, ds4424_of_match);

static struct i2c_driver ds4424_driver = {
	.driver = {
		.name	= "ds4424",
		.of_match_table = ds4424_of_match,
		.pm     = pm_sleep_ptr(&ds4424_pm_ops),
	},
	.probe		= ds4424_probe,
	.remove		= ds4424_remove,
	.id_table	= ds4424_id,
};
module_i2c_driver(ds4424_driver);

MODULE_DESCRIPTION("Maxim DS4424 DAC Driver");
MODULE_AUTHOR("Ismail H. Kose <ismail.kose@maximintegrated.com>");
MODULE_AUTHOR("Vishal Sood <vishal.sood@maximintegrated.com>");
MODULE_AUTHOR("David Jung <david.jung@maximintegrated.com>");
MODULE_LICENSE("GPL v2");
