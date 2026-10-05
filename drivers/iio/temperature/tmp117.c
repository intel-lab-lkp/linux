// SPDX-License-Identifier: GPL-2.0-only
/*
 * Digital temperature sensor with integrated Non-volatile memory
 * Copyright (c) 2021 Puranjay Mohan <puranjay12@gmail.com>
 *
 * Driver for the Texas Instruments TMP117 Temperature Sensor
 * (7-bit I2C slave address (0x48 - 0x4B), changeable via ADD pins)
 *
 * Note: This driver assumes that the sensor has been calibrated beforehand.
 */

#include <linux/array_size.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#include <linux/iio/iio.h>
#include <linux/iio/events.h>

#define TMP117_REG_TEMP			0x0
#define TMP117_REG_CFGR			0x1
#define TMP117_CFGR_HIGH_ALERT		BIT(15)
#define TMP117_CFGR_LOW_ALERT		BIT(14)
#define TMP117_REG_HIGH_LIM		0x2
#define TMP117_REG_LOW_LIM		0x3
#define TMP117_REG_EEPROM_UL		0x4
#define TMP117_REG_EEPROM1		0x5
#define TMP117_REG_EEPROM2		0x6
#define TMP117_REG_TEMP_OFFSET		0x7
#define TMP117_REG_EEPROM3		0x8
#define TMP117_REG_DEVICE_ID		0xF

#define TMP117_RESOLUTION_10UC		78125
#define MICRODEGREE_PER_10MILLIDEGREE	10000

/*
 * The temperature limit registers store a signed 16-bit value with an LSB of
 * TMP117_RESOLUTION_10UC / MICRODEGREE_PER_10MILLIDEGREE = 125/16 m°C.
 */
#define TMP117_LIMIT_LSB_NUM		125
#define TMP117_LIMIT_LSB_DEN		16

#define TMP116_DEVICE_ID		0x1116
#define TMP117_DEVICE_ID		0x0117
#define TMP119_DEVICE_ID		0x2117

struct tmp117_data {
	struct i2c_client *client;
	s16 calibbias;
};

struct tmp11x_info {
	const char *name;
	struct iio_chan_spec const *channels;
	int num_channels;
};

static irqreturn_t tmp117_interrupt_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct tmp117_data *data = iio_priv(indio_dev);
	int ret;

	ret = i2c_smbus_read_word_swapped(data->client, TMP117_REG_CFGR);
	if (ret < 0 || !(ret & (TMP117_CFGR_HIGH_ALERT | TMP117_CFGR_LOW_ALERT)))
		return IRQ_NONE;

	if (ret & TMP117_CFGR_HIGH_ALERT)
		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(IIO_TEMP, 0,
						    IIO_EV_TYPE_THRESH,
						    IIO_EV_DIR_RISING),
			       iio_get_time_ns(indio_dev));

	if (ret & TMP117_CFGR_LOW_ALERT)
		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(IIO_TEMP, 0,
						    IIO_EV_TYPE_THRESH,
						    IIO_EV_DIR_FALLING),
			       iio_get_time_ns(indio_dev));

	return IRQ_HANDLED;
}

static int tmp117_read_thresh(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan,
			      enum iio_event_type type,
			      enum iio_event_direction dir,
			      enum iio_event_info info,
			      int *val, int *val2)
{
	struct tmp117_data *data = iio_priv(indio_dev);
	s32 ret;
	u8 reg;

	switch (dir) {
	case IIO_EV_DIR_RISING:
		reg = TMP117_REG_HIGH_LIM;
		break;
	case IIO_EV_DIR_FALLING:
		reg = TMP117_REG_LOW_LIM;
		break;
	default:
		return -EINVAL;
	}

	if (info != IIO_EV_INFO_VALUE || type != IIO_EV_TYPE_THRESH)
		return -EINVAL;
	ret = i2c_smbus_read_word_swapped(data->client, reg);
	if (ret < 0)
		return ret;

	/*
	 * Report the threshold in millidegrees Celsius as an exact fraction:
	 * value = raw * TMP117_LIMIT_LSB_NUM / TMP117_LIMIT_LSB_DEN.
	 */
	*val = sign_extend32(ret, 15) * TMP117_LIMIT_LSB_NUM;
	*val2 = TMP117_LIMIT_LSB_DEN;

	return IIO_VAL_FRACTIONAL;
}

static int tmp117_write_thresh(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       enum iio_event_type type,
			       enum iio_event_direction dir,
			       enum iio_event_info info,
			       int val, int val2)
{
	struct tmp117_data *data = iio_priv(indio_dev);
	s64 tmp;
	u8 reg;

	switch (dir) {
	case IIO_EV_DIR_RISING:
		reg = TMP117_REG_HIGH_LIM;
		break;
	case IIO_EV_DIR_FALLING:
		reg = TMP117_REG_LOW_LIM;
		break;
	default:
		return -EINVAL;
	}

	if (info != IIO_EV_INFO_VALUE || type != IIO_EV_TYPE_THRESH)
		return -EINVAL;

	/*
	 * The threshold is passed in millidegrees Celsius as
	 * value = val + val2 / 1000000 (val2 holds the fractional part, with
	 * the sign taken from val). Convert it back with the same LSB ratio:
	 * raw = value / (TMP117_LIMIT_LSB_NUM / TMP117_LIMIT_LSB_DEN).
	 */
	tmp = (val < 0) ? (s64)val * 1000000 - val2
			: (s64)val * 1000000 + val2;
	tmp = DIV_ROUND_CLOSEST(tmp * TMP117_LIMIT_LSB_DEN,
				(s64)TMP117_LIMIT_LSB_NUM * 1000000);
	tmp = clamp_t(s64, tmp, S16_MIN, S16_MAX);

	return i2c_smbus_write_word_swapped(data->client,
					    reg, tmp);
}

static int tmp117_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *channel, int *val,
			   int *val2, long mask)
{
	struct tmp117_data *data = iio_priv(indio_dev);
	s32 ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = i2c_smbus_read_word_swapped(data->client,
						  TMP117_REG_TEMP);
		if (ret < 0)
			return ret;
		*val = sign_extend32(ret, 15);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_CALIBBIAS:
		ret = i2c_smbus_read_word_swapped(data->client,
						  TMP117_REG_TEMP_OFFSET);
		if (ret < 0)
			return ret;
		*val = sign_extend32(ret, 15);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		/*
		 * Conversion from 10s of uC to mC
		 * as IIO reports temperature in mC
		 */
		*val = TMP117_RESOLUTION_10UC / MICRODEGREE_PER_10MILLIDEGREE;
		*val2 = (TMP117_RESOLUTION_10UC %
					MICRODEGREE_PER_10MILLIDEGREE) * 100;

		return IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

static int tmp117_write_raw(struct iio_dev *indio_dev, struct iio_chan_spec
			    const *channel, int val, int val2, long mask)
{
	struct tmp117_data *data = iio_priv(indio_dev);
	s16 off;

	switch (mask) {
	case IIO_CHAN_INFO_CALIBBIAS:
		off = clamp_t(int, val, S16_MIN, S16_MAX);
		if (off == data->calibbias)
			return 0;
		data->calibbias = off;
		return i2c_smbus_write_word_swapped(data->client,
						TMP117_REG_TEMP_OFFSET, off);

	default:
		return -EINVAL;
	}
}

static const struct iio_event_spec tmp11x_obj_event[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	},
};

static const struct iio_chan_spec tmp117_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_CALIBBIAS) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.event_spec = tmp11x_obj_event,
		.num_event_specs = ARRAY_SIZE(tmp11x_obj_event),
	},
};

static const struct iio_chan_spec tmp116_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.event_spec = tmp11x_obj_event,
		.num_event_specs = ARRAY_SIZE(tmp11x_obj_event),
	},
};

static const struct tmp11x_info tmp116_channels_info = {
	.name = "tmp116",
	.channels = tmp116_channels,
	.num_channels = ARRAY_SIZE(tmp116_channels)
};

static const struct tmp11x_info tmp117_channels_info = {
	.name = "tmp117",
	.channels = tmp117_channels,
	.num_channels = ARRAY_SIZE(tmp117_channels)
};

static const struct tmp11x_info tmp119_channels_info = {
	.name = "tmp119",
	.channels = tmp117_channels,
	.num_channels = ARRAY_SIZE(tmp117_channels)
};

static const struct iio_info tmp117_info = {
	.read_raw = tmp117_read_raw,
	.write_raw = tmp117_write_raw,
	.read_event_value = tmp117_read_thresh,
	.write_event_value = tmp117_write_thresh,
};

static int tmp117_probe(struct i2c_client *client)
{
	const struct tmp11x_info *match_data;
	struct tmp117_data *data;
	struct iio_dev *indio_dev;
	int dev_id;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_WORD_DATA))
		return -EOPNOTSUPP;

	ret = devm_regulator_get_enable(&client->dev, "vcc");
	if (ret)
		return ret;

	fsleep(1500);

	dev_id = i2c_smbus_read_word_swapped(client, TMP117_REG_DEVICE_ID);
	if (dev_id < 0)
		return dev_id;

	switch (dev_id) {
	case TMP116_DEVICE_ID:
		match_data = &tmp116_channels_info;
		break;
	case TMP117_DEVICE_ID:
		match_data = &tmp117_channels_info;
		break;
	case TMP119_DEVICE_ID:
		match_data = &tmp119_channels_info;
		break;
	default:
		dev_info(&client->dev,
			 "Unknown device id (0x%x), use fallback compatible\n",
			 dev_id);
		match_data = i2c_get_match_data(client);
	}

	if (!match_data)
		return dev_err_probe(&client->dev, -ENODEV,
				     "Failed to identify unsupported device\n");

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	data->calibbias = 0;

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &tmp117_info;
	indio_dev->channels = match_data->channels;
	indio_dev->num_channels = match_data->num_channels;
	indio_dev->name = match_data->name;

	if (client->irq) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, tmp117_interrupt_handler,
						IRQF_ONESHOT, match_data->name,
						indio_dev);
		if (ret) {
			dev_err_probe(&client->dev, ret, "irq request error\n");
			return ret;
		}
	}

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct of_device_id tmp117_of_match[] = {
	{ .compatible = "ti,tmp116", .data = &tmp116_channels_info },
	{ .compatible = "ti,tmp117", .data = &tmp117_channels_info },
	{ .compatible = "ti,tmp119", .data = &tmp119_channels_info },
	{ }
};
MODULE_DEVICE_TABLE(of, tmp117_of_match);

static const struct i2c_device_id tmp117_id[] = {
	{ .name = "tmp116", .driver_data = (kernel_ulong_t)&tmp116_channels_info },
	{ .name = "tmp117", .driver_data = (kernel_ulong_t)&tmp117_channels_info },
	{ .name = "tmp119", .driver_data = (kernel_ulong_t)&tmp119_channels_info },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tmp117_id);

static struct i2c_driver tmp117_driver = {
	.driver = {
		.name	= "tmp117",
		.of_match_table = tmp117_of_match,
	},
	.probe		= tmp117_probe,
	.id_table	= tmp117_id,
};
module_i2c_driver(tmp117_driver);

MODULE_AUTHOR("Puranjay Mohan <puranjay12@gmail.com>");
MODULE_DESCRIPTION("TI TMP117 Temperature sensor driver");
MODULE_LICENSE("GPL");
