// SPDX-License-Identifier: GPL-2.0+
/*
 * mpm369x.c  - pmbus driver for mps mpm369x
 *
 * Copyright 2026 Monolithic Power Systems, Inc
 *
 * Author: Yuxi Wang <Yuxi.Wang@monolithicpower.com>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pmbus.h>
#include "pmbus.h"

#define PAGE	0x01
#define MPM369x_FUNC	(PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | \
			 PMBUS_HAVE_IOUT | PMBUS_HAVE_TEMP | \
			 PMBUS_HAVE_STATUS_VOUT | PMBUS_HAVE_STATUS_IOUT | \
			 PMBUS_HAVE_STATUS_INPUT | PMBUS_HAVE_STATUS_TEMP)

static int mpm369x_read_byte_data(struct i2c_client *client, int page,
				  int reg)
{
	return -ENODATA;
}

static int mpm369x_read_word_data(struct i2c_client *client, int page, int phase,
				  int reg)
{
	int ret;

	switch (reg) {
	case PMBUS_READ_TEMPERATURE_1:
		ret = pmbus_read_word_data(client, page, phase, reg);
		if (ret < 0)
			return ret;
		/*
		 * Because the Temperature format used by the chip is a 2-byte,
		 * twos complement integer and the sign bit is bits[9].
		 * Report that signed short integer.
		 */
		if (ret & 0x200)
			ret = (ret & 0x1ff) | 0xfe00;
		break;
	default:
		ret = -ENODATA;
		break;
	}
	return ret;
}

static struct pmbus_driver_info mpm369x_info = {
	.pages = PAGE,
	.format[PSC_VOLTAGE_IN] = direct,
	.format[PSC_CURRENT_OUT] = direct,
	.format[PSC_VOLTAGE_OUT] = direct,
	.format[PSC_TEMPERATURE] = direct,

	.m[PSC_VOLTAGE_IN] = 40,
	.b[PSC_VOLTAGE_IN] = 0,
	.R[PSC_VOLTAGE_IN] = 0,

	.m[PSC_CURRENT_OUT] = 16,
	.b[PSC_CURRENT_OUT] = 0,
	.R[PSC_CURRENT_OUT] = 0,

	.m[PSC_VOLTAGE_OUT] = 800,
	.b[PSC_VOLTAGE_OUT] = 0,
	.R[PSC_VOLTAGE_OUT] = 0,

	.m[PSC_TEMPERATURE] = 1,
	.b[PSC_TEMPERATURE] = 0,
	.R[PSC_TEMPERATURE] = 3,

	.read_word_data = mpm369x_read_word_data,
	.read_byte_data = mpm369x_read_byte_data,
	.func[0] = MPM369x_FUNC,
};

static int mpm369x_probe(struct i2c_client *client)
{
	return pmbus_do_probe(client, &mpm369x_info);
}

static const struct i2c_device_id mpm369x_id[] = {
	{ "MPM3695-20", 0 },
	{ "MPM3690S-15", 1 },
	{}
};
MODULE_DEVICE_TABLE(i2c, mpm369x_id);

static const struct of_device_id mpm369x_of_match[] = {
	{ .compatible = "mps,mpm3695-20" },
	{ .compatible = "mps,mpm3690S-15" },
	{}
};
MODULE_DEVICE_TABLE(of, mpm369x_of_match);

static struct i2c_driver mpm369x_driver = {
	.probe = mpm369x_probe,
	.driver = {
			.name = "mpm369x",
			.of_match_table = mpm369x_of_match,
		   },
	.id_table = mpm369x_id,
};

module_i2c_driver(mpm369x_driver);
MODULE_AUTHOR("Yuxi Wang <Yuxi.Wang@monolithicpower.com>");
MODULE_DESCRIPTION("MPS MPM369x pmbus driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("PMBUS");
