// SPDX-License-Identifier: GPL-2.0+
/*
 * Hardware monitoring driver for Texas Instruments INA233
 *
 * Copyright (c) 2017 Google Inc
 *
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include "pmbus.h"

#define MFR_CALIBRATION	0xd4

struct pmbus_driver_info ina233_info = {
	.pages = 1,
	.format[PSC_VOLTAGE_IN] = direct,
	.format[PSC_VOLTAGE_OUT] = direct,
	.format[PSC_CURRENT_IN] = direct,
	.format[PSC_CURRENT_OUT] = direct,
	.format[PSC_POWER] = direct,
	.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_STATUS_INPUT
	    | PMBUS_HAVE_VOUT | PMBUS_HAVE_IOUT
		| PMBUS_HAVE_STATUS_IOUT | PMBUS_HAVE_POUT,
	.m[PSC_VOLTAGE_IN] = 8,
	.m[PSC_VOLTAGE_OUT] = 8,
	.R[PSC_VOLTAGE_IN] = 2,
	.R[PSC_VOLTAGE_OUT] = 2,
};

static int ina233_probe(struct i2c_client *client)
{
	int ret;
	u16 shunt;
	u16 current_lsb;
	of_property_read_u16(client->dev.of_node, "resistor-calibration", &shunt);

	ret = i2c_smbus_write_word_data(client, MFR_CALIBRATION, shunt);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to set calibration\n");
		return ret;
	}
	ret = of_property_read_u16(client->dev.of_node, "current-lsb", &current_lsb);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to set current_lsb\n");
		return ret;
	} else {
		// Referenced by table of Telemetryand WarningConversionCoefficients in datasheet
		ina233_info.m[PSC_CURRENT_IN] = 1000 / current_lsb;
		ina233_info.m[PSC_CURRENT_OUT] = 1000 / current_lsb;
		ina233_info.m[PSC_POWER] = 40 / current_lsb;
	}

	return pmbus_do_probe(client, &ina233_info);
}

static const struct i2c_device_id ina233_id[] = {
	{"ina233", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, ina233_id);

static const struct of_device_id __maybe_unused ina233_of_match[] = {
	{ .compatible = "ti,ina233" },
	{}
};

MODULE_DEVICE_TABLE(of, ina233_of_match);

static struct i2c_driver ina233_driver = {
	.driver = {
		   .name = "ina233",
		   .of_match_table = of_match_ptr(ina233_of_match),
		   },
	.probe_new = ina233_probe,
	.id_table = ina233_id,
};

module_i2c_driver(ina233_driver);

MODULE_AUTHOR("Eli Huang <eli_huang@wiwynn.com>");
MODULE_DESCRIPTION("PMBus driver for Texas Instruments INA233 and compatible chips");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(PMBUS);

