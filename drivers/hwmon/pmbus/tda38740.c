// SPDX-License-Identifier: GPL-2.0+
/*
 * Hardware monitoring driver for Infineon TDA38725/TDA38740
 *
 * Copyright (c) 2023 9elements GmbH
 *
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/property.h>
#include "pmbus.h"

#define TDA38740_VOUT_SCALE_DEFAULT_MICRO	1000000
#define TDA38740_VOUT_SCALE_MIN_MICRO		100000
#define TDA38740_VOUT_SCALE_MAX_MICRO		2000000

struct tda38740_data {
	struct pmbus_driver_info info;
	u32 vout_scale_micro;
};

static int tda38740_read_word_data(struct i2c_client *client, int page,
				   int phase, int reg)
{
	const struct tda38740_data *data;
	int ret;
	u64 scaled;

	if (reg != PMBUS_READ_VOUT)
		return -ENODATA;

	ret = pmbus_read_word_data(client, page, phase, reg);
	if (ret < 0)
		return ret;

	data = container_of(pmbus_get_driver_info(client), struct tda38740_data,
			    info);

	scaled = (u64)ret * data->vout_scale_micro;
	scaled = DIV_ROUND_CLOSEST_ULL(scaled,
				       TDA38740_VOUT_SCALE_DEFAULT_MICRO);

	return clamp_val(scaled, 0, U16_MAX);
}

/*
 * TDA38725/TDA38740 only support Linear format for VOUT related commands,
 * with exponents in the range of -8 to -12 (see datasheet VOUT_MODE
 * description). Direct format is not supported by this device.
 */
static int tda38740_identify(struct i2c_client *client,
			     struct pmbus_driver_info *info)
{
	int vout_mode;

	vout_mode = pmbus_read_byte_data(client, 0, PMBUS_VOUT_MODE);
	if (vout_mode < 0 || vout_mode == 0xff)
		return vout_mode < 0 ? vout_mode : -ENODEV;

	if ((vout_mode >> 5) != 0)
		return -ENODEV;

	info->format[PSC_VOLTAGE_OUT] = linear;

	return 0;
}

static struct pmbus_driver_info tda38740_info = {
	.pages = 1,
	.format[PSC_VOLTAGE_IN] = linear,
	.format[PSC_CURRENT_OUT] = linear,
	.format[PSC_CURRENT_IN] = linear,
	.format[PSC_POWER] = linear,
	.format[PSC_TEMPERATURE] = linear,
	.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_STATUS_INPUT
	    | PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_TEMP
	    | PMBUS_HAVE_IIN
	    | PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT
	    | PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT
	    | PMBUS_HAVE_POUT | PMBUS_HAVE_PIN,
	.identify = tda38740_identify,
};

static int tda38740_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tda38740_data *data;
	const char *propname;
	u32 vout_scale_micro;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	propname = "infineon,vout-scale-micro";
	if (device_property_present(dev, propname)) {
		ret = device_property_read_u32(dev, propname, &vout_scale_micro);
		if (ret)
			return dev_err_probe(dev, ret,
					     "%s property read fail.\n",
					     propname);
	} else {
		vout_scale_micro = TDA38740_VOUT_SCALE_DEFAULT_MICRO;
	}

	if (vout_scale_micro < TDA38740_VOUT_SCALE_MIN_MICRO ||
	    vout_scale_micro > TDA38740_VOUT_SCALE_MAX_MICRO)
		return -EINVAL;

	memcpy(&data->info, &tda38740_info, sizeof(tda38740_info));
	data->vout_scale_micro = vout_scale_micro;
	data->info.read_word_data = tda38740_read_word_data;

	return pmbus_do_probe(client, &data->info);
}

static const struct i2c_device_id tda38740_id[] = {
	{ .name = "tda38725"},
	{ .name = "tda38740"},
	{}
};
MODULE_DEVICE_TABLE(i2c, tda38740_id);

static const struct of_device_id __maybe_unused tda38740_of_match[] = {
	{ .compatible = "infineon,tda38725"},
	{ .compatible = "infineon,tda38740"},
	{ },
};
MODULE_DEVICE_TABLE(of, tda38740_of_match);

/* This is the driver that will be inserted */
static struct i2c_driver tda38740_driver = {
	.driver = {
		.name = "tda38740",
		.of_match_table = of_match_ptr(tda38740_of_match),
	},
	.probe = tda38740_probe,
	.id_table = tda38740_id,
};

module_i2c_driver(tda38740_driver);

MODULE_DESCRIPTION("PMBus driver for Infineon TDA38725/TDA38740");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("PMBUS");
