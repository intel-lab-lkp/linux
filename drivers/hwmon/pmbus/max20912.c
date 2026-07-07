// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hardware monitoring driver for Analog Devices MAX20912 and MAX20916
 * Dual-Output Voltage Regulator
 *
 * Copyright (c) 2026 Quanta Computer Inc.
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include "pmbus.h"

static struct pmbus_driver_info max20912_info = {
	.pages = 2,
	.format[PSC_VOLTAGE_IN] = linear,
	.format[PSC_VOLTAGE_OUT] = vid,
	.vrm_version[0] = vr12,
	.vrm_version[1] = vr12,
	.format[PSC_TEMPERATURE] = linear,
	.format[PSC_CURRENT_IN] = linear,
	.format[PSC_CURRENT_OUT] = linear,
	.format[PSC_POWER] = linear,
	.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_IIN | PMBUS_HAVE_STATUS_INPUT |
		PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT |
		PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT |
		PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_TEMP |
		PMBUS_HAVE_PIN | PMBUS_HAVE_POUT,
	.func[1] = PMBUS_HAVE_IIN | PMBUS_HAVE_STATUS_INPUT |
		PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT |
		PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT |
		PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_TEMP |
		PMBUS_HAVE_PIN | PMBUS_HAVE_POUT,
};

static int max20912_probe(struct i2c_client *client)
{
	return pmbus_do_probe(client, &max20912_info);
}

static const struct of_device_id max20912_of_match[] = {
	{ .compatible = "adi,max20912" },
	{ .compatible = "adi,max20916" },
	{}
};
MODULE_DEVICE_TABLE(of, max20912_of_match);

static const struct i2c_device_id max20912_id[] = {
	{"max20912"},
	{"max20916"},
	{}
};

MODULE_DEVICE_TABLE(i2c, max20912_id);

static struct i2c_driver max20912_driver = {
	.driver = {
		.name = "max20912",
		.of_match_table = max20912_of_match,
	},
	.probe = max20912_probe,
	.id_table = max20912_id,
};

module_i2c_driver(max20912_driver);

MODULE_AUTHOR("Fred Chen <fredchen.openbmc@gmail.com>");
MODULE_DESCRIPTION("PMBus driver for Analog Devices MAX20912 and MAX20916");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("PMBUS");
