// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tsi.c - Side band TSI over I2C support for AMD out of band management.
 *
 * Copyright (c) 2020-2026, Google Inc.
 * Copyright (c) 2020-2026, Kun Yi <kunyi@google.com>
 */

#include <linux/bitfield.h>
#include "tsi-core.h"

#define SBTSI_REG_CONFIG		0x03 /* RO */

/*
 * Bit for reporting value with temperature measurement range.
 * bit == 0: Use default temperature range (0C to 255.875C).
 * bit == 1: Use extended temperature range (-49C to +206.875C).
 */
#define SBTSI_CONFIG_EXT_RANGE_SHIFT   2

/*
 * ReadOrder bit specifies the reading order of integer and decimal part of
 * CPU temperature for atomic reads. If bit == 0, reading integer part triggers
 * latching of the decimal part, so integer part should be read first.
 */

#define SBTSI_CONFIG_READ_ORDER_SHIFT  5

static int sbtsi_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sbtsi_data *data;
	int err;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	err = i2c_smbus_read_byte_data(data->client, SBTSI_REG_CONFIG);
	if (err < 0)
		return err;
	data->ext_range_mode = FIELD_GET(BIT(SBTSI_CONFIG_EXT_RANGE_SHIFT), err);
	data->read_order = FIELD_GET(BIT(SBTSI_CONFIG_READ_ORDER_SHIFT), err);

	dev_set_drvdata(dev, data);
	return create_sbtsi_hwmon_sensor_device(dev, data);
}

static const struct i2c_device_id sbtsi_id[] = {
	{"sbtsi"},
	{}
};
MODULE_DEVICE_TABLE(i2c, sbtsi_id);

static const struct of_device_id __maybe_unused sbtsi_of_match[] = {
	{
		.compatible = "amd,sbtsi",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, sbtsi_of_match);

static struct i2c_driver sbtsi_driver = {
	.driver = {
		.name = "sbtsi",
		.of_match_table = of_match_ptr(sbtsi_of_match),
	},
	.probe = sbtsi_probe,
	.id_table = sbtsi_id,
};

module_i2c_driver(sbtsi_driver);

MODULE_AUTHOR("Kun Yi <kunyi@google.com>");
MODULE_DESCRIPTION("Hwmon driver for AMD SB-TSI emulated sensor");
MODULE_LICENSE("GPL");
