// SPDX-License-Identifier: GPL-2.0-only
/*
 * Maxim Integrated
 * 7-bit, Multi-Channel Sink/Source Current DAC Driver
 * Copyright (C) 2017 Maxim Integrated
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/iio/driver.h>
#include <linux/iio/machine.h>

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

/*
 * Two variant groups share the same register map but differ in:
 * - resolution/data mask (DS4402/DS4404: 5-bit, DS4422/DS4424: 7-bit)
 * - full-scale current calculation (different Vref and divider)
 * Addressing also differs (DS440x tri-level, DS442x bi-level), but is
 * handled via board configuration, not driver logic.
 */
struct ds4424_chip_info {
	u8 result_mask;
	int vref_mv;
	int scale_denom;
};

static const struct ds4424_chip_info ds4424_info = {
	.result_mask = 0x7F,
	.vref_mv = 976,
	.scale_denom = 16,
};

/* DS4402 is handled like DS4404 (same resolution and scale formula). */
static const struct ds4424_chip_info ds4404_info = {
	.result_mask = 0x1F,
	.vref_mv = 1230,
	.scale_denom = 4,
};

struct ds4424_data {
	struct i2c_client *client;
	struct mutex lock;
	uint8_t save[DS4424_MAX_DAC_CHANNELS];
	struct regulator *vcc_reg;
	uint8_t raw[DS4424_MAX_DAC_CHANNELS];
	const struct ds4424_chip_info *chip_info;
	u32 rfs_ohms[DS4424_MAX_DAC_CHANNELS];
	bool has_rfs;
};

static const struct iio_chan_spec ds4424_channels[] = {
	DS4424_CHANNEL(0),
	DS4424_CHANNEL(1),
	DS4424_CHANNEL(2),
	DS4424_CHANNEL(3),
};

static int ds4424_get_value(struct iio_dev *indio_dev,
			     int *val, int channel)
{
	struct ds4424_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = i2c_smbus_read_byte_data(data->client, DS4424_DAC_ADDR(channel));
	if (ret < 0)
		goto fail;

	*val = ret;

fail:
	mutex_unlock(&data->lock);
	return ret;
}

static int ds4424_set_value(struct iio_dev *indio_dev,
			     int val, struct iio_chan_spec const *chan)
{
	struct ds4424_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = i2c_smbus_write_byte_data(data->client,
			DS4424_DAC_ADDR(chan->channel), val);
	if (ret < 0)
		goto fail;

	data->raw[chan->channel] = val;

fail:
	mutex_unlock(&data->lock);
	return ret;
}

static int ds4424_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	union ds4424_raw_data raw;
	struct ds4424_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = ds4424_get_value(indio_dev, val, chan->channel);
		if (ret < 0) {
			dev_err_ratelimited(&indio_dev->dev,
					    "%s: ds4424_get_value failed %pe\n",
					    __func__, ERR_PTR(ret));
			return ret;
		}
		raw.bits = *val;
		*val = raw.dx & data->chip_info->result_mask;
		if (raw.source_bit == DS4424_SINK_I)
			*val = -*val;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		if (!data->has_rfs)
			return -EINVAL;

		/* SCALE is mA/step: mV / Ohm = mA. */
		*val = data->chip_info->vref_mv;
		*val2 = data->rfs_ohms[chan->channel] *
			data->chip_info->scale_denom;
		return IIO_VAL_FRACTIONAL;

	default:
		return -EINVAL;
	}
}

static int ds4424_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct ds4424_data *data = iio_priv(indio_dev);
	int max_val = data->chip_info->result_mask;
	union ds4424_raw_data raw;

	if (val2 != 0)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/*
		 * The hardware uses sign-magnitude representation (not
		 * two's complement). Therefore, the range is symmetric:
		 * [-max_val, +max_val].
		 */
		if (val < -max_val || val > max_val)
			return -EINVAL;

		/* Canonicalize 0 to sink; datasheet treats sign as don't-care. */
		if (val > 0) {
			raw.source_bit = DS4424_SOURCE_I;
			raw.dx = val;
		} else {
			raw.source_bit = DS4424_SINK_I;
			raw.dx = -val;
		}

		return ds4424_set_value(indio_dev, raw.bits, chan);

	default:
		return -EINVAL;
	}
}

static int ds4424_verify_chip(struct iio_dev *indio_dev)
{
	int ret, val;

	/* No device ID; verify presence by a readable register. */
	ret = ds4424_get_value(indio_dev, &val, 0);
	if (ret < 0)
		dev_err(&indio_dev->dev,
				"%s failed. ret: %d\n", __func__, ret);

	return ret;
}

static int ds4424_init(struct iio_dev *indio_dev)
{
	int i, ret;

	/* Set all channels to 0 current. */
	for (i = 0; i < indio_dev->num_channels; i++) {
		ret = ds4424_set_value(indio_dev, 0, &indio_dev->channels[i]);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int ds4424_setup_channels(struct i2c_client *client,
				 struct ds4424_data *data,
				 struct iio_dev *indio_dev)
{
	struct iio_chan_spec channels[DS4424_MAX_DAC_CHANNELS];
	size_t channels_size;
	int i;

	channels_size = indio_dev->num_channels * sizeof(*channels);
	memcpy(channels, ds4424_channels, channels_size);

	/* Enable scale only when rfs is available. */
	if (data->has_rfs) {
		for (i = 0; i < indio_dev->num_channels; i++)
			channels[i].info_mask_separate |=
				BIT(IIO_CHAN_INFO_SCALE);
	}

	indio_dev->channels = devm_kmemdup(&client->dev, channels,
					   channels_size, GFP_KERNEL);
	if (!indio_dev->channels)
		return -ENOMEM;

	return 0;
}

static int ds4424_parse_rfs(struct i2c_client *client,
			    struct ds4424_data *data,
			    struct iio_dev *indio_dev)
{
	int count, i, ret;

	if (!device_property_present(&client->dev, "maxim,rfs-ohms")) {
		dev_info_once(&client->dev, "maxim,rfs-ohms missing, scale not supported\n");
		return 0;
	}

	count = device_property_count_u32(&client->dev, "maxim,rfs-ohms");
	if (count != indio_dev->num_channels) {
		dev_err(&client->dev,
			"maxim,rfs-ohms must have %u entries\n",
			indio_dev->num_channels);
		return -EINVAL;
	}

	ret = device_property_read_u32_array(&client->dev,
					     "maxim,rfs-ohms",
					     data->rfs_ohms,
					     indio_dev->num_channels);
	if (ret) {
		dev_err(&client->dev,
			"Failed to read maxim,rfs-ohms property\n");
		return ret;
	}

	for (i = 0; i < indio_dev->num_channels; i++) {
		if (!data->rfs_ohms[i]) {
			dev_err(&client->dev,
				"maxim,rfs-ohms entry %d is zero\n",
				i);
			return -EINVAL;
		}
	}

	data->has_rfs = true;
	return 0;
}

static int ds4424_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ds4424_data *data = iio_priv(indio_dev);
	int ret = 0;
	int i;

	for (i = 0; i < indio_dev->num_channels; i++) {
		data->save[i] = data->raw[i];
		ret = ds4424_set_value(indio_dev, 0,
				&indio_dev->channels[i]);
		if (ret < 0)
			return ret;
	}
	return ret;
}

static int ds4424_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ds4424_data *data = iio_priv(indio_dev);
	int ret = 0;
	int i;

	for (i = 0; i < indio_dev->num_channels; i++) {
		ret = ds4424_set_value(indio_dev, data->save[i],
				&indio_dev->channels[i]);
		if (ret < 0)
			return ret;
	}
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ds4424_pm_ops, ds4424_suspend, ds4424_resume);

static const struct iio_info ds4424_iio_info = {
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
	data->client = client;
	indio_dev->name = id->name;

	data->vcc_reg = devm_regulator_get(&client->dev, "vcc");
	if (IS_ERR(data->vcc_reg))
		return dev_err_probe(&client->dev, PTR_ERR(data->vcc_reg),
				     "Failed to get vcc-supply regulator.\n");

	mutex_init(&data->lock);
	ret = regulator_enable(data->vcc_reg);
	if (ret < 0) {
		dev_err(&client->dev,
				"Unable to enable the regulator.\n");
		return ret;
	}

	usleep_range(1000, 1200);
	ret = ds4424_verify_chip(indio_dev);
	if (ret < 0)
		goto fail;

	switch (id->driver_data) {
	case ID_DS4402:
		indio_dev->num_channels = DS4422_MAX_DAC_CHANNELS;
		/* See ds4404_info comment above. */
		data->chip_info = &ds4404_info;
		break;
	case ID_DS4404:
		indio_dev->num_channels = DS4424_MAX_DAC_CHANNELS;
		data->chip_info = &ds4404_info;
		break;
	case ID_DS4422:
		indio_dev->num_channels = DS4422_MAX_DAC_CHANNELS;
		data->chip_info = &ds4424_info;
		break;
	case ID_DS4424:
		indio_dev->num_channels = DS4424_MAX_DAC_CHANNELS;
		data->chip_info = &ds4424_info;
		break;
	default:
		dev_err(&client->dev,
				"ds4424: Invalid chip id.\n");
		ret = -ENXIO;
		goto fail;
	}

	ret = ds4424_parse_rfs(client, data, indio_dev);
	if (ret)
		goto fail;

	ret = ds4424_setup_channels(client, data, indio_dev);
	if (ret)
		goto fail;

	/* No reset pin/bit: clear any preconfigured output on probe. */
	ret = ds4424_init(indio_dev);
	if (ret)
		goto fail;

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ds4424_iio_info;

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
