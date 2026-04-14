// SPDX-License-Identifier: GPL-2.0
/*
 * Hardware monitoring driver for Analog Devices MAX20830
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 */

#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include "pmbus.h"

static struct pmbus_driver_info max20830_info = {
	.pages = 1,
	.format[PSC_VOLTAGE_IN] = linear,
	.format[PSC_VOLTAGE_OUT] = linear,
	.format[PSC_CURRENT_OUT] = linear,
	.format[PSC_TEMPERATURE] = linear,
	.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_IOUT |
		PMBUS_HAVE_TEMP |
		PMBUS_HAVE_STATUS_VOUT | PMBUS_HAVE_STATUS_IOUT |
		PMBUS_HAVE_STATUS_INPUT | PMBUS_HAVE_STATUS_TEMP,
};

static int max20830_probe(struct i2c_client *client)
{
	u8 buf[I2C_SMBUS_BLOCK_MAX + 1];
	int ret;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_READ_I2C_BLOCK))
		return -ENODEV;

	ret = i2c_smbus_read_i2c_block_data(client, PMBUS_IC_DEVICE_ID,
					    I2C_SMBUS_BLOCK_MAX, buf);
	if (ret < 0)
		return dev_err_probe(&client->dev, ret,
				     "Failed to read IC_DEVICE_ID\n");

	buf[ret] = '\0';
	if (ret < 9 || strncmp(buf + 1, "MAX20830", 8))
		return dev_err_probe(&client->dev, -ENODEV,
				     "Unsupported device: '%s'\n", buf);

	return pmbus_do_probe(client, &max20830_info);
}

static const struct i2c_device_id max20830_id[] = {
	{"max20830"},
	{ }
};
MODULE_DEVICE_TABLE(i2c, max20830_id);

static const struct of_device_id max20830_of_match[] = {
	{ .compatible = "adi,max20830" },
	{ }
};
MODULE_DEVICE_TABLE(of, max20830_of_match);

static struct i2c_driver max20830_driver = {
	.driver = {
		.name = "max20830",
		.of_match_table = max20830_of_match,
	},
	.probe = max20830_probe,
	.id_table = max20830_id,
};

module_i2c_driver(max20830_driver);

MODULE_AUTHOR("Alexis Czezar Torreno <alexisczezar.torreno@analog.com>");
MODULE_DESCRIPTION("PMBus driver for Analog Devices MAX20830");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(PMBUS);
