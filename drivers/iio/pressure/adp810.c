// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Akhilesh Patil <akhilesh@ee.iitb.ac.in>
 *
 * Driver for adp810 pressure and temperature sensor
 * Datasheet:
 *   https://aosong.com/userfiles/files/media/Datasheet%20ADP810-Digital.pdf
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/crc8.h>

/* Time taken in ms by sensor to do measurements after triggering.
 * As per datahseet, 10ms is sufficient but we define 30 for better margin
 */
#define ADP810_MEASURE_LATENCY		30
/* Trigger command to send to start measurement by the sensor */
#define ADP810_TRIGGER_COMMAND		0x2d37
#define ADP810_CRC8_POLYNOMIAL		0x31

DECLARE_CRC8_TABLE(crc_table);

struct adp810_read_buf {
	u8 dp_msb;
	u8 dp_lsb;
	u8 dp_crc;
	u8 tmp_msb;
	u8 tmp_lsb;
	u8 tmp_crc;
	u8 sf_msb;
	u8 sf_lsb;
	u8 sf_crc;
} __packed;

struct adp810_data {
	struct i2c_client *client;
	/* Use lock to synchronize access to device during read sequence */
	struct mutex lock;
};

static int adp810_measure(struct adp810_data *data, struct adp810_read_buf *buf)
{
	struct i2c_client *client = data->client;
	int ret;
	u16 trig_cmd = ADP810_TRIGGER_COMMAND;

	/* Send trigger to the sensor for measurement */
	ret = i2c_master_send(client, (char *)&trig_cmd, sizeof(u16));
	if (ret < 0) {
		dev_err(&client->dev, "Error sending trigger command\n");
		return ret;
	}

	/* Wait for sensor to aquire data */
	msleep(ADP810_MEASURE_LATENCY);

	/* Read sensor values */
	ret = i2c_master_recv(client, (char *)buf, sizeof(*buf));
	if (ret < 0) {
		dev_err(&client->dev, "Error reading from sensor\n");
		return ret;
	}

	/* CRC checks */
	crc8_populate_msb(crc_table, ADP810_CRC8_POLYNOMIAL);
	if (buf->dp_crc != crc8(crc_table, &buf->dp_msb, 0x2, CRC8_INIT_VALUE)) {
		dev_err(&client->dev, "CRC error for pressure\n");
		return -EIO;
	}

	if (buf->tmp_crc != crc8(crc_table, &buf->tmp_msb, 0x2, CRC8_INIT_VALUE)) {
		dev_err(&client->dev, "CRC error for temperature\n");
		return -EIO;
	}

	if (buf->sf_crc != crc8(crc_table, &buf->sf_msb, 0x2, CRC8_INIT_VALUE)) {
		dev_err(&client->dev, "CRC error for scale\n");
		return -EIO;
	}

	return 0;
}

static int adp810_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct adp810_data *data = iio_priv(indio_dev);
	struct adp810_read_buf buf = {0};
	int ret;

	mutex_lock(&data->lock);
	ret = adp810_measure(data, &buf);
	mutex_unlock(&data->lock);

	if (ret) {
		dev_err(&indio_dev->dev, "Failed to read from device\n");
		return ret;
	}

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_PRESSURE:
			*val = buf.dp_msb << 8 | buf.dp_lsb;
			return IIO_VAL_INT;
		case IIO_TEMP:
			*val = buf.tmp_msb << 8 | buf.tmp_lsb;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_PRESSURE:
			*val = buf.sf_msb << 8 | buf.sf_lsb;
			return IIO_VAL_INT;
		case IIO_TEMP:
			*val = 200;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}

	return -EINVAL;
}

static const struct iio_info adp810_info = {
	.read_raw	= adp810_read_raw,
};

static const struct iio_chan_spec adp810_channels[] = {
	{
		.type = IIO_PRESSURE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
	},
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int adp810_probe(struct i2c_client *client)
{
	const struct i2c_device_id *dev_id = i2c_client_get_device_id(client);
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct adp810_data *data;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	mutex_init(&data->lock);

	indio_dev->name = dev_id->name;
	indio_dev->channels = adp810_channels;
	indio_dev->num_channels = ARRAY_SIZE(adp810_channels);
	indio_dev->info = &adp810_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register IIO device\n");

	return ret;
}

static const struct i2c_device_id adp810_id_table[] = {
	{ "adp810" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, adp810_id_table);

static const struct of_device_id adp810_of_table[] = {
	{ .compatible = "aosong,adp810" },
	{ }
};
MODULE_DEVICE_TABLE(of, adp810_of_table);

static struct i2c_driver adp810_driver = {
	.driver = {
		.name = "adp810",
		.of_match_table = adp810_of_table,
	},
	.probe	= adp810_probe,
	.id_table = adp810_id_table,
};
module_i2c_driver(adp810_driver);

MODULE_AUTHOR("Akhilesh Patil <akhilesh@ee.iitb.ac.in>");
MODULE_DESCRIPTION("Driver for Aosong ADP810 sensor");
MODULE_LICENSE("GPL");
