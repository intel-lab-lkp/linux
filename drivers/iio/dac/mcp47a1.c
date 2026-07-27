// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip MCP47A1 DAC driver
 *
 * Copyright (c) 2026 Joshua Crofts <joshua.crofts1@gmail.com>
 *
 * Datasheet: https://ww1.microchip.com/downloads/aemDocuments/documents/OTH/ProductDocuments/DataSheets/25154A.pdf
 */

#include <linux/array_size.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/units.h>

#include <linux/iio/iio.h>

#define MCP47A1_REG_MAX		0x40
#define MCP47A1_CMD_CODE	0x00
#define MCP47A1_MAX_STEP	63

struct mcp47a1_data {
	struct i2c_client *client;
	int vref_mV;
};

static const int mcp47a1_raw_avail[] = { 0, 1, MCP47A1_MAX_STEP };

static const struct iio_chan_spec mcp47a1_channels[] = {
	{
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int mcp47a1_write(struct iio_dev *indio_dev,
			 struct iio_chan_spec const *chan,
			 int val, int val2, long mask)
{
	struct mcp47a1_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (val < 0 || val > MCP47A1_MAX_STEP)
			return -EINVAL;

		return i2c_smbus_write_byte_data(data->client, MCP47A1_CMD_CODE,
						 val);
	default:
		return -EINVAL;
	}
}

static int mcp47a1_read(struct iio_dev *indio_dev,
			struct iio_chan_spec const *chan,
			int *val, int *val2, long mask)
{
	struct mcp47a1_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = i2c_smbus_read_byte_data(data->client, MCP47A1_CMD_CODE);
		if (ret < 0)
			return ret;

		*val = ret;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = data->vref_mV;
		*val2 = MCP47A1_MAX_STEP;

		return IIO_VAL_FRACTIONAL;
	default:
		return -EINVAL;
	}
}

static int mcp47a1_read_avail(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      const int **vals, int *type, int *length,
			      long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		*vals = mcp47a1_raw_avail;
		*type = IIO_VAL_INT;
		return IIO_AVAIL_RANGE;
	default:
		return -EINVAL;
	}
}

static const struct iio_info mcp47a1_info = {
	.write_raw = mcp47a1_write,
	.read_raw = mcp47a1_read,
	.read_avail = mcp47a1_read_avail,
};

static int mcp47a1_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct mcp47a1_data *data;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable regulator\n");

	/* Delay after device exits reset state (see AC/DC characteristics) */
	fsleep(20);

	ret = devm_regulator_get_enable_read_voltage(dev, "vref");
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to read vref\n");

	data->vref_mV = ret / (MICRO / MILLI);

	indio_dev->name = "mcp47a1";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &mcp47a1_info;
	indio_dev->channels = mcp47a1_channels;
	indio_dev->num_channels = ARRAY_SIZE(mcp47a1_channels);

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id mcp47a1_of_match[] = {
	{ .compatible = "microchip,mcp47a1" },
	{ }
};
MODULE_DEVICE_TABLE(of, mcp47a1_of_match);

static const struct i2c_device_id mcp47a1_id[] = {
	{ .name = "mcp47a1" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mcp47a1_id);

static struct i2c_driver mcp47a1_driver = {
	.driver = {
		.name = "mcp47a1",
		.of_match_table = mcp47a1_of_match,
	},
	.probe = mcp47a1_probe,
	.id_table = mcp47a1_id,
};
module_i2c_driver(mcp47a1_driver);

MODULE_AUTHOR("Joshua Crofts <joshua.crofts1@gmail.com>");
MODULE_DESCRIPTION("MCP47A1 DAC");
MODULE_LICENSE("GPL");
