// SPDX-License-Identifier: GPL-2.0
/*
 * Hardware monitoring driver for Analog Devices ADP1050
 *
 * Copyright (C) 2024 Analog Devices, Inc.
 */
#include <linux/bits.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include "pmbus.h"

#define ADP1050_CHIP_PASSWORD		0xD7

#define ADP1050_VIN_SCALE_MONITOR	0xD8
#define ADP1050_IIN_SCALE_MONITOR	0xD9

static struct pmbus_driver_info adp1050_info = {
	.pages = 1,
	.format[PSC_VOLTAGE_IN] = linear,
	.format[PSC_VOLTAGE_OUT] = linear,
	.format[PSC_CURRENT_IN] = linear,
	.format[PSC_TEMPERATURE] = linear,
	.func[0] = PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT
		| PMBUS_HAVE_VIN | PMBUS_HAVE_STATUS_INPUT
		| PMBUS_HAVE_IIN | PMBUS_HAVE_TEMP
		| PMBUS_HAVE_STATUS_TEMP,
};

static int adp1050_probe(struct i2c_client *client)
{
	u32 vin_scale_monitor, iin_scale_monitor;
	int ret;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_WRITE_WORD_DATA))
		return -ENODEV;

	/* Unlock CHIP's password in order to be able to read/write to it's
	 * VIN_SCALE and IIN_SCALE registers.
	*/
	ret = i2c_smbus_write_word_data(client, ADP1050_CHIP_PASSWORD, 0xFFFF);
	if (ret < 0) {
		dev_err_probe(&client->dev, "Device can't be unlocked.\n");
		return ret;
	}

	ret = i2c_smbus_write_word_data(client, ADP1050_CHIP_PASSWORD, 0xFFFF);
	if (ret < 0) {
		dev_err_probe(&client->dev, "Device couldn't be unlocked.\n");
		return ret;
	}

	/* If adi,vin-scale-monitor isn't set or is set to 0 means that the
	 * VIN monitor isn't used, therefore 0 is used as scale in order
	 * for the readings to return 0.
	*/
	if (device_property_read_u32(&client->dev, "adi,vin-scale-monitor",
				     &vin_scale_monitor))
		vin_scale_monitor = 0;

	/* If adi,iin-scale-monitor isn't set or is set to 0 means that the
	 * IIN monitor isn't used, therefore 0 is used as scale in order
	 * for the readings to return 0.
	*/
	if (device_property_read_u32(&client->dev, "adi,iin-scale-monitor",
				     &iin_scale_monitor))
		iin_scale_monitor = 0;

	ret = i2c_smbus_write_word_data(client, ADP1050_VIN_SCALE_MONITOR,
					vin_scale_monitor);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_write_word_data(client, ADP1050_IIN_SCALE_MONITOR,
					iin_scale_monitor);
	if (ret < 0)
		return ret;

	return pmbus_do_probe(client, &adp1050_info);
}

static const struct i2c_device_id adp1050_id[] = {
	{"adp1050", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, adp1050_id);

static const struct of_device_id adp1050_of_match[] = {
	{ .compatible = "adi,adp1050"},
	{}
};
MODULE_DEVICE_TABLE(of, adp1050_of_match);

static struct i2c_driver adp1050_driver = {
	.driver = {
		.name = "adp1050",
		.of_match_table = of_match_ptr(adp1050_of_match),
	},
	.probe = adp1050_probe,
	.id_table = adp1050_id,
};
module_i2c_driver(adp1050_driver);

MODULE_AUTHOR("Radu Sabau <radu.sabau@analog.com>");
MODULE_DESCRIPTION("Analog Devices ADP1050 HWMON PMBus Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(PMBUS);
