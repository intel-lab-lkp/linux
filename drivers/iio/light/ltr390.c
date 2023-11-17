// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * IIO driver for Lite-On LTR390 ALS and UV sensor
 * (7-bit I2C slave address 0x53)
 *
 * Based on the work of:
 *   Shreeya Patel and Shi Zhigang (LTRF216 Driver)
 *
 * Copyright (C) 2023 Anshul Dalal <anshulusr@gmail.com>
 *
 * Datasheet:
 *   https://optoelectronics.liteon.com/upload/download/DS86-2015-0004/LTR-390UV_Final_%20DS_V1%201.pdf
 *
 * TODO:
 *   - Support for configurable gain and resolution
 *   - Sensor suspend/resume support
 *   - Add support for reading the ALS
 *   - Interrupt support
 */

#include <asm/unaligned.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>

#define LTR390_DEVICE_NAME	    "ltr390"

#define LTR390_MAIN_CTRL	    0x00
#define LTR390_PART_ID		    0x06
#define LTR390_UVS_DATA		    0x10

#define LTR390_SW_RESET		    BIT(4)
#define LTR390_UVS_MODE		    BIT(3)
#define LTR390_SENSOR_ENABLE	    BIT(1)

#define LTR390_PART_NUMBER_ID	    0xb
#define LTR390_FRACTIONAL_PERCISION 100

/*
 * At 20-bit resolution (integration time: 400ms) and 18x gain, 2300 counts of
 * the sensor are equal to 1 UV Index [Datasheet Page#8].
 *
 * For the default resolution of 18-bit (integration time: 100ms) and default
 * gain of 3x, the counts/uvi are calculated as follows:
 * 2300 / ((3/18) * (100/400)) = 95.83
 */
#define LTR390_COUNTS_PER_UVI 96

/*
 * Window Factor is needed when the device is under Window glass with coated
 * tinted ink. This is to compensate for the light loss due to the lower
 * transmission rate of the window glass and helps * in calculating lux.
 */
#define LTR390_WINDOW_FACTOR 1

struct ltr390_data {
	struct regmap *regmap;
	struct i2c_client *client;
	struct mutex lock;
};

static const struct regmap_config ltr390_regmap_config = {
	.name = LTR390_DEVICE_NAME,
	.reg_bits = 8,
	.reg_stride = 1,
	.val_bits = 8,
};

static int ltr390_register_read(struct ltr390_data *data, u8 register_address)
{
	struct device *dev = &data->client->dev;
	int ret;
	u8 recieve_buffer[3];

	mutex_lock(&data->lock);

	ret = regmap_bulk_read(data->regmap, register_address, recieve_buffer,
			       sizeof(recieve_buffer));
	if (ret) {
		dev_err(dev, "failed to read measurement data: %d\n", ret);
		mutex_unlock(&data->lock);
		return ret;
	}

	mutex_unlock(&data->lock);
	return get_unaligned_le24(recieve_buffer);
}

static int ltr390_get_uv_index(struct ltr390_data *data)
{
	int ret;
	int uv_index;

	ret = ltr390_register_read(data, LTR390_UVS_DATA);
	if (ret < 0)
		return ret;

	uv_index = DIV_ROUND_CLOSEST(ret * LTR390_FRACTIONAL_PERCISION *
					     LTR390_WINDOW_FACTOR,
				     LTR390_COUNTS_PER_UVI);

	return uv_index;
}

static int ltr390_read_raw(struct iio_dev *iio_device,
			   struct iio_chan_spec const *chan, int *val,
			   int *val2, long mask)
{
	int ret;
	struct ltr390_data *data = iio_priv(iio_device);

	switch (mask) {
	case IIO_CHAN_INFO_PROCESSED:
		ret = ltr390_get_uv_index(data);
		if (ret < 0)
			return ret;
		*val = ret;
		*val2 = LTR390_FRACTIONAL_PERCISION;
		return IIO_VAL_FRACTIONAL;
	case IIO_CHAN_INFO_RAW:
		ret = ltr390_register_read(data, LTR390_UVS_DATA);
		if (ret < 0)
			return ret;
		*val = ret;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_info ltr390_info = {
	.read_raw = ltr390_read_raw,
};

static const struct iio_chan_spec ltr390_channels[] = {
	{
		.type = IIO_UVINDEX,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED)
	},
	{
		.type = IIO_INTENSITY,
		.channel2 = IIO_MOD_LIGHT_UV,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW)
	},
};

static int ltr390_probe(struct i2c_client *client)
{
	struct ltr390_data *data;
	struct iio_dev *indio_dev;
	int ret, part_number;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);

	data->regmap = devm_regmap_init_i2c(client, &ltr390_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(data->regmap),
				     "regmap initialization failed\n");

	data->client = client;
	i2c_set_clientdata(client, indio_dev);
	mutex_init(&data->lock);

	indio_dev->info = &ltr390_info;
	indio_dev->channels = ltr390_channels;
	indio_dev->num_channels = ARRAY_SIZE(ltr390_channels);
	indio_dev->name = LTR390_DEVICE_NAME;

	ret = regmap_read(data->regmap, LTR390_PART_ID, &part_number);
	if (ret) {
		dev_err(&client->dev, "failed to get sensor's part id: %d",
			ret);
		return ret;
	}
	/* Lower 4 bits of `part_number` change with hardware revisions */
	if (part_number >> 4 != LTR390_PART_NUMBER_ID) {
		dev_err(&client->dev, "received invalid product id: 0x%x",
			part_number);
		return -ENODEV;
	}
	dev_dbg(&client->dev, "LTR390, product id: 0x%x\n", part_number);

	/* reset sensor, chip fails to respond to this, so ignore any errors */
	regmap_set_bits(data->regmap, LTR390_MAIN_CTRL, LTR390_SW_RESET);

	/* Wait for the registers to reset before proceeding */
	usleep_range(1000, 2000);

	ret = regmap_set_bits(data->regmap, LTR390_MAIN_CTRL,
			      LTR390_SENSOR_ENABLE | LTR390_UVS_MODE);
	if (ret) {
		dev_err(&client->dev, "failed to enable the sensor: %d\n", ret);
		return ret;
	}

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct i2c_device_id ltr390_id[] = {
	{ LTR390_DEVICE_NAME, 0 },
	{ /* Sentinel */ },
};
MODULE_DEVICE_TABLE(i2c, ltr390_id);

static const struct of_device_id ltr390_of_table[] = {
	{ .compatible = "liteon,ltr390"},
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, ltr390_id_table);

static struct i2c_driver ltr390_driver = {
	.driver = {
		.name = LTR390_DEVICE_NAME,
		.of_match_table = ltr390_of_table,
	},
	.probe = ltr390_probe,
	.id_table = ltr390_id,
};

module_i2c_driver(ltr390_driver);

MODULE_AUTHOR("Anshul Dalal <anshulusr@gmail.com>");
MODULE_DESCRIPTION("Lite-On LTR390 ALS and UV sensor Driver");
MODULE_LICENSE("GPL");
