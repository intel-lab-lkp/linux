// SPDX-License-Identifier: GPL-2.0-only
/*
 * Honeywell TruStability HSC Series pressure/temperature sensor
 *
 * Copyright (c) 2023 Petre Rodan <petre.rodan@subdimension.ro>
 *
 * Datasheet: https://prod-edam.honeywell.com/content/dam/honeywell-edam/sps/siot/en-us/products/sensors/pressure-sensors/board-mount-pressure-sensors/trustability-hsc-series/documents/sps-siot-trustability-hsc-series-high-accuracy-board-mount-pressure-sensors-50099148-a-en-ciid-151133.pdf [hsc]
 * Datasheet: https://prod-edam.honeywell.com/content/dam/honeywell-edam/sps/siot/en-us/products/sensors/pressure-sensors/board-mount-pressure-sensors/common/documents/sps-siot-i2c-comms-digital-output-pressure-sensors-tn-008201-3-en-ciid-45841.pdf [i2c related]
 */

#include <linux/i2c.h>
#include <linux/module.h>

#include <linux/iio/iio.h>

#include "hsc030pa.h"

static int hsc_i2c_xfer(struct hsc_data *data)
{
	struct i2c_client *client = data->client;
	struct i2c_msg msg;
	int ret;

	msg.addr = client->addr;
	msg.flags = client->flags | I2C_M_RD;
	msg.len = HSC_REG_MEASUREMENT_RD_SIZE;
	msg.buf = (char *)&data->buffer;

	ret = i2c_transfer(client->adapter, &msg, 1);

	return (ret == 2) ? 0 : ret;
}

static int hsc_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct hsc_data *hsc;
	const struct i2c_device_id *id = i2c_client_get_device_id(client);

	indio_dev = devm_iio_device_alloc(dev, sizeof(*hsc));
	if (!indio_dev)
		return -ENOMEM;

	hsc = iio_priv(indio_dev);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	hsc->xfer = hsc_i2c_xfer;
	hsc->client = client;

	return hsc_probe(indio_dev, &client->dev, id->name, id->driver_data);
}

static const struct of_device_id hsc_i2c_match[] = {
	{.compatible = "honeywell,hsc030pa"},
	{}
};
MODULE_DEVICE_TABLE(of, hsc_i2c_match);

static const struct i2c_device_id hsc_i2c_id[] = {
	{"hsc030pa"},
	{}
};
MODULE_DEVICE_TABLE(i2c, hsc_i2c_id);

static struct i2c_driver hsc_i2c_driver = {
	.driver = {
		.name = "hsc030pa",
		.of_match_table = hsc_i2c_match,
		},
	.probe = hsc_i2c_probe,
	.id_table = hsc_i2c_id,
};
module_i2c_driver(hsc_i2c_driver);

MODULE_AUTHOR("Petre Rodan <petre.rodan@subdimension.ro>");
MODULE_DESCRIPTION("Honeywell HSC and SSC pressure sensor i2c driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_HONEYWELL_HSC030PA);
