// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for MPS MP5990 Hot-Swap Controller
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pmbus.h>
#include "pmbus.h"

static int mp5990_read_byte_data(struct i2c_client *client, int page, int reg)
{
	switch (reg) {
	case PMBUS_VOUT_MODE:
		/*
		  Enforce VOUT direct format, C4h reg BIT9
		  default val is not match vout format
		 */
		return PB_VOUT_MODE_DIRECT;
	default:
		return -ENODATA;
	}
}

static struct pmbus_driver_info mp5990_info = {
	.pages = 1,
	.format[PSC_VOLTAGE_IN] = direct,
	.format[PSC_VOLTAGE_OUT] = direct,
	.format[PSC_CURRENT_OUT] = direct,
	.format[PSC_POWER] = direct,
	.format[PSC_TEMPERATURE] = direct,
	.m[PSC_VOLTAGE_IN] = 32,
	.b[PSC_VOLTAGE_IN] = 0,
	.R[PSC_VOLTAGE_IN] = 0,
	.m[PSC_VOLTAGE_OUT] = 32,
	.b[PSC_VOLTAGE_OUT] = 0,
	.R[PSC_VOLTAGE_OUT] = 0,
	.m[PSC_CURRENT_OUT] = 16,
	.b[PSC_CURRENT_OUT] = 0,
	.R[PSC_CURRENT_OUT] = 0,
	.m[PSC_POWER] = 1,
	.b[PSC_POWER] = 0,
	.R[PSC_POWER] = 0,
	.m[PSC_TEMPERATURE] = 1,
	.b[PSC_TEMPERATURE] = 0,
	.R[PSC_TEMPERATURE] = 0,
	.func[0] =
		PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_PIN |
		PMBUS_HAVE_TEMP | PMBUS_HAVE_IOUT |
		PMBUS_HAVE_STATUS_INPUT | PMBUS_HAVE_STATUS_TEMP,
	.read_byte_data = mp5990_read_byte_data,
};

static int mp5990_probe(struct i2c_client *client)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, PMBUS_VOUT_MODE,
					PB_VOUT_MODE_DIRECT);
	if (ret < 0)
		return ret;
	return pmbus_do_probe(client, &mp5990_info);
}

static const struct of_device_id mp5990_of_match[] = {
	{ .compatible = "mps,mp5990" },
	{}
};

static const struct i2c_device_id mp5990_id[] = {
	{"mp5990", 0},
	{ }
};
MODULE_DEVICE_TABLE(i2c, mp5990_id);

static struct i2c_driver mp5990_driver = {
	.driver = {
		   .name = "mp5990",
		   .of_match_table = of_match_ptr(mp5990_of_match),
	},
	.probe = mp5990_probe,
	.id_table = mp5990_id,
};
module_i2c_driver(mp5990_driver);

MODULE_AUTHOR("Peter Yin <peter.yin@quantatw.com>");
MODULE_DESCRIPTION("PMBus driver for MP5990 HSC");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(PMBUS);
