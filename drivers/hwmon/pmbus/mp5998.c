// SPDX-License-Identifier: GPL-2.0+
/*
 * mp5998.c  - pmbus driver for mps mp5998
 *
 * Copyright 2025 Monolithic Power Systems, Inc
 *
 * Author: Yuxi Wang <Yuxi.Wang@monolithicpower.com>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include "pmbus.h"

/*Common Register*/
#define PAGE	0x01
#define EFUSE_CFG 0xC4
#define MP5998_FUNC	(PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | \
					PMBUS_HAVE_IOUT | PMBUS_HAVE_IIN | \
					PMBUS_HAVE_PIN | PMBUS_HAVE_POUT | \
					PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_IOUT | \
					PMBUS_HAVE_STATUS_INPUT | PMBUS_HAVE_STATUS_TEMP)

static int mp5998_read_word_data(struct i2c_client *client, int page,
				 int phase, int reg)
{
	int ret;

	switch (reg) {
	case PMBUS_READ_VIN...PMBUS_READ_TEMPERATURE_1:
	case PMBUS_READ_POUT...PMBUS_READ_PIN:
	case PMBUS_STATUS_WORD:
		ret = -ENODATA;
	break;
	default:
		ret = -EINVAL;
	break;
	}

	return ret;
}

static int mp5998_read_byte_data(struct i2c_client *client, int page,
				 int reg)
{
	int ret;

	switch (reg) {
	case PMBUS_STATUS_BYTE:
	case PMBUS_STATUS_IOUT:
	case PMBUS_STATUS_INPUT:
	case PMBUS_STATUS_TEMPERATURE:
	case PMBUS_STATUS_CML:
	case PMBUS_STATUS_MFR_SPECIFIC:
	   ret = -ENODATA;
	break;
	default:
		ret = -EINVAL;
	break;
	}
	return ret;
}

static struct pmbus_driver_info mp5998_info_linear = {
	.pages = PAGE,
	.format[PSC_VOLTAGE_IN] = linear,
	.format[PSC_CURRENT_IN] = linear,
	.format[PSC_VOLTAGE_OUT] = linear,
	.format[PSC_CURRENT_OUT] = linear,
	.format[PSC_TEMPERATURE] = linear,
	.format[PSC_POWER] = linear,

	.read_word_data = mp5998_read_word_data,
	.read_byte_data = mp5998_read_byte_data,
	.func[0] = MP5998_FUNC,
};

static struct pmbus_driver_info mp5998_info_direct = {
	.pages = PAGE,
	.format[PSC_VOLTAGE_IN] = direct,
	.format[PSC_CURRENT_IN] = direct,
	.format[PSC_VOLTAGE_OUT] = direct,
	.format[PSC_CURRENT_OUT] = direct,
	.format[PSC_TEMPERATURE] = direct,
	.format[PSC_POWER] = direct,

	.m[PSC_VOLTAGE_IN] = 64,
	.b[PSC_VOLTAGE_IN] = 0,
	.R[PSC_VOLTAGE_IN] = 0,

	.m[PSC_CURRENT_IN] = 16,
	.b[PSC_CURRENT_IN] = 0,
	.R[PSC_CURRENT_IN] = 0,

	.m[PSC_VOLTAGE_OUT] = 64,
	.b[PSC_VOLTAGE_OUT] = 0,
	.R[PSC_VOLTAGE_OUT] = 0,

	.m[PSC_CURRENT_OUT] = 16,
	.b[PSC_CURRENT_OUT] = 0,
	.R[PSC_CURRENT_OUT] = 0,

	.m[PSC_TEMPERATURE] = 1,
	.b[PSC_TEMPERATURE] = 0,
	.R[PSC_TEMPERATURE] = 3,

	.m[PSC_POWER] = 2,
	.b[PSC_POWER] = 0,
	.R[PSC_POWER] = 0,

	.read_word_data = mp5998_read_word_data,
	.read_byte_data = mp5998_read_byte_data,
	.func[0] = MP5998_FUNC,
};

static int mp5998_probe(struct i2c_client *client)
{
	int ret;

	ret = i2c_smbus_read_word_data(client, EFUSE_CFG);

	if (ret < 0)
		return ret;

	if (ret & BIT(9))
		ret = pmbus_do_probe(client, &mp5998_info_linear);
	else
		ret = pmbus_do_probe(client, &mp5998_info_direct);

	if (!ret)
		dev_info(&client->dev, "%s chip found\n", client->name);
	return ret;
}

static const struct i2c_device_id mp5998_id[] = {
	{ "mp5998", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, mp5998_id);

static const struct of_device_id mp5998_of_match[] = {
	{ .compatible = "mps,mp5998" },
	{}
};
MODULE_DEVICE_TABLE(of, mp5998_of_match);

static struct i2c_driver mp5998_driver = {
	.probe = mp5998_probe,
	.driver = {
			.name = "mp5998",
			.of_match_table = mp5998_of_match,
		   },
	.id_table = mp5998_id,
};

module_i2c_driver(mp5998_driver);
MODULE_AUTHOR("Yuxi Wang <Yuxi.Wang@monolithicpower.com>");
MODULE_DESCRIPTION("MPS MP5998 HWMON driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("PMBUS");
