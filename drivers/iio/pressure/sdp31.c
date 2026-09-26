// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/cleanup.h>
#include <linux/crc8.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/once.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

#include <linux/iio/iio.h>

#define SDP31_CMD_TRIG_DP     0x362F
#define SDP31_MEAS_DELAY_MS   50
#define SDP31_POWERUP_TIME_MS 25   /* datasheet t_PU (max) */
#define SDP31_CRC8_POLY       0x31
#define SDP31_CRC8_INIT       0xff

DECLARE_CRC8_TABLE(sdp31_crc8_table);

struct sdp31_data {
	struct i2c_client *client;
	struct mutex lock; /* serializes access to the sensor */
	u16 dp_scale;
};

struct sdp31_reading {
	s16 pressure;
	s16 temp;
	u16 scale;
};

static int sdp31_send_cmd(struct i2c_client *client, u16 cmd)
{
	__be16 buf = cpu_to_be16(cmd);
	int ret;

	ret = i2c_master_send(client, (u8 *)&buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return (ret == sizeof(buf)) ? 0 : -EIO;
}

static int sdp31_check_crc(const u8 *word)
{
	if (crc8(sdp31_crc8_table, word, 2, SDP31_CRC8_INIT) != word[2])
		return -EIO;

	return 0;
}

static int sdp31_measure(struct sdp31_data *data, struct sdp31_reading *out)
{
	u8 rx[9];
	int ret;

	guard(mutex)(&data->lock);

	ret = sdp31_send_cmd(data->client, SDP31_CMD_TRIG_DP);
	if (ret)
		return ret;

	msleep(SDP31_MEAS_DELAY_MS);

	ret = i2c_master_recv(data->client, rx, sizeof(rx));
	if (ret < 0)
		return ret;
	if (ret != sizeof(rx))
		return -EIO;

	if (sdp31_check_crc(&rx[0]) ||
	    sdp31_check_crc(&rx[3]) ||
	    sdp31_check_crc(&rx[6]))
		return -EIO;

	out->pressure = (s16)get_unaligned_be16(&rx[0]);
	out->temp = (s16)get_unaligned_be16(&rx[3]);
	out->scale = get_unaligned_be16(&rx[6]);

	return 0;
}

static int sdp31_read_raw(struct iio_dev *indio_dev,
			  struct iio_chan_spec const *chan,
			  int *val, int *val2, long mask)
{
	struct sdp31_data *data = iio_priv(indio_dev);
	struct sdp31_reading r;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = sdp31_measure(data, &r);
		if (ret)
			return ret;
		switch (chan->type) {
		case IIO_PRESSURE:
			*val = r.pressure;
			return IIO_VAL_INT;
		case IIO_TEMP:
			*val = r.temp;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_PRESSURE:
			*val = 1;
			*val2 = data->dp_scale * 1000;
			return IIO_VAL_FRACTIONAL;
		case IIO_TEMP:
			/* raw / 200 = degC, so raw * 5 = milli-degC */
			*val = 5;
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static const struct iio_info sdp31_info = {
	.read_raw = sdp31_read_raw,
};

static const struct iio_chan_spec sdp31_channels[] = {
	{
		.type = IIO_PRESSURE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int sdp31_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct sdp31_data *data;
	struct sdp31_reading r;
	int ret;

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable regulator\n");

	/* Wait for the sensor to be ready after power-up (datasheet t_PU). */
	msleep(SDP31_POWERUP_TIME_MS);

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;

	ret = devm_mutex_init(dev, &data->lock);
	if (ret)
		return ret;

	/* The CRC table is shared by all instances; initialise it once. */
	DO_ONCE(crc8_populate_msb, sdp31_crc8_table, SDP31_CRC8_POLY);

	/* Confirm the sensor is present and learn its scale factor. */
	ret = sdp31_measure(data, &r);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read from sensor\n");
	if (!r.scale)
		return dev_err_probe(dev, -EINVAL, "invalid scale factor\n");
	data->dp_scale = r.scale;

	indio_dev->name = "sdp31";
	indio_dev->info = &sdp31_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = sdp31_channels;
	indio_dev->num_channels = ARRAY_SIZE(sdp31_channels);

	return devm_iio_device_register(dev, indio_dev);
}

static const struct i2c_device_id sdp31_id[] = {
	{ .name = "sdp31" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sdp31_id);

static const struct of_device_id sdp31_of_match[] = {
	{ .compatible = "sensirion,sdp31" },
	{ }
};
MODULE_DEVICE_TABLE(of, sdp31_of_match);

static struct i2c_driver sdp31_driver = {
	.driver = {
		.name = "sdp31",
		.of_match_table = sdp31_of_match,
	},
	.probe = sdp31_probe,
	.id_table = sdp31_id,
};
module_i2c_driver(sdp31_driver);

MODULE_AUTHOR("Muhammad Abu Bakar");
MODULE_DESCRIPTION("Sensirion SDP31 differential pressure sensor");
MODULE_LICENSE("GPL");
