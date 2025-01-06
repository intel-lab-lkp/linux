// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hardware monitoring driver for ina233
 *
 * Copyright (c) 2024 Leo Yang
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include "pmbus.h"

#define MFR_READ_VSHUNT 0xd1
#define MFR_CALIBRATION 0xd4

#define INA233_RSHUNT_DEFAULT		2000 /* uOhm */
#define INA233_CURRENT_LSB_DEFAULT	1000 /* uA/bit */

#define MAX_M_VAL 32767
#define MIN_M_VAL -32768

static int calculate_coef(int *m, int *R, bool power)
{
	s64 scaled_m;
	int scale_factor = 0;
	int scale_coef = 1;
	int power_coef = 1;
	bool is_integer = false;

	if (*m == 0) {
		*R = 0;
		return -1;
	}

	if (power)
		power_coef = 25;

	if (1000000 % *m) {
		/* Default value, Scaling to keep integer precision,
		 * Change it if you need
		 */
		scale_factor = -3;
		scale_coef = 1000;
	} else {
		is_integer = true;
	}

	/*
	 * Unit Conversion (Current_LSB A->uA) and use scaling(scale_factor)
	 * to keep integer precision.
	 * Formulae referenced from spec.
	 */
	scaled_m = div_s64(1000000 * scale_coef, *m * power_coef);

	/* Maximize while keeping it bounded.*/
	while (scaled_m > MAX_M_VAL || scaled_m < MIN_M_VAL) {
		scaled_m /= 10;
		scale_factor++;
	}
	/* Scale up only if fractional part exists. */
	while (scaled_m * 10 < MAX_M_VAL && scaled_m * 10 > MIN_M_VAL && !is_integer) {
		scaled_m *= 10;
		scale_factor--;
	}

	*m = scaled_m;
	*R = scale_factor;
	return 0;
}

static int ina233_read_word_data(struct i2c_client *client, int page,
				 int phase, int reg)
{
	int ret;

	switch (reg) {
	case PMBUS_VIRT_READ_VMON:
		ret = pmbus_read_word_data(client, 0, 0xff, MFR_READ_VSHUNT);

		/* Adjust returned value to match VIN coefficients */
		/* VIN: 1.25 mV VSHUNT: 2.5 uV LSB */
		ret = DIV_ROUND_CLOSEST(ret * 25, 12500);
		break;
	default:
		ret = -ENODATA;
		break;
	}
	return ret;
}

struct pmbus_driver_info ina233_info = {
	.pages = 1,
	.format[PSC_VOLTAGE_IN] = direct,
	.format[PSC_VOLTAGE_OUT] = direct,
	.format[PSC_CURRENT_OUT] = direct,
	.format[PSC_POWER] = direct,
	.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_INPUT
		| PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT
		| PMBUS_HAVE_POUT
		| PMBUS_HAVE_VMON | PMBUS_HAVE_STATUS_VMON,
	.m[PSC_VOLTAGE_IN] = 8,
	.R[PSC_VOLTAGE_IN] = 2,
	.m[PSC_VOLTAGE_OUT] = 8,
	.R[PSC_VOLTAGE_OUT] = 2,
	.read_word_data = ina233_read_word_data,
};

static int ina233_probe(struct i2c_client *client)
{
	int ret, m, R;
	u32 rshunt;
	u16 current_lsb;
	u16 calibration;

	/* If INA233 skips current/power, shunt-resistor and current-lsb aren't needed.	*/

	/* read rshunt value (uOhm) */
	ret = of_property_read_u32(client->dev.of_node, "shunt-resistor", &rshunt);
	if (ret < 0 || !rshunt) {
		dev_err(&client->dev, "Unable to read shunt-resistor or value is 0, default value %d uOhm is used.\n",
			INA233_RSHUNT_DEFAULT);
		rshunt = INA233_RSHUNT_DEFAULT;
	}

	/* read current_lsb value (uA/bit) */
	ret = of_property_read_u16(client->dev.of_node, "current-lsb", &current_lsb);
	if (ret < 0 || !current_lsb) {
		dev_err(&client->dev, "Unable to read current_lsb or value is 0, default value %d uA/bit is used.\n",
			INA233_CURRENT_LSB_DEFAULT);
		current_lsb = INA233_CURRENT_LSB_DEFAULT;
	}

	/* calculate current coefficient */
	m = current_lsb;
	ret = calculate_coef(&m, &R, false);
	if (ret < 0) {
		dev_err(&client->dev, "Calculate_coef error\n");
	} else {
		ina233_info.m[PSC_CURRENT_OUT] = m;
		ina233_info.R[PSC_CURRENT_OUT] = R;
	}

	/* calculate power coefficient */
	m = current_lsb;
	ret = calculate_coef(&m, &R, true);
	if (ret < 0) {
		dev_err(&client->dev, "Calculate_coef error\n");
	} else {
		ina233_info.m[PSC_POWER] = m;
		ina233_info.R[PSC_POWER] = R;
	}

	/* write MFR_CALIBRATION register, Apply formula from spec with unit scaling. */
	calibration = div_u64((u64)5120000000, (u64)rshunt * current_lsb);
	if (calibration <= 0) {
		dev_err(&client->dev, "Calibration error\n");
		return -1;
	}
	ret = i2c_smbus_write_word_data(client, MFR_CALIBRATION, calibration);
	if (ret < 0) {
		dev_err(&client->dev, "Unable to write calibration\n");
		return ret;
	}

	dev_info(&client->dev, "power monitor %s (Rshunt = %u uOhm, Current_LSB = %u uA/bit)\n",
		 client->name, rshunt, current_lsb);

	return pmbus_do_probe(client, &ina233_info);
}

static const struct i2c_device_id ina233_id[] = {
	{"ina233", 0},
	{ }
};
MODULE_DEVICE_TABLE(i2c, ina233_id);

static const struct of_device_id __maybe_unused ina233_of_match[] = {
	{ .compatible = "ti,ina233" },
	{ },
};
MODULE_DEVICE_TABLE(of, ina233_of_match);

/* This is the driver that will be inserted */
static struct i2c_driver ina233_driver = {
	.driver = {
		   .name = "ina233",
		   .of_match_table = of_match_ptr(ina233_of_match),
	},
	.probe = ina233_probe,
	.id_table = ina233_id,
};

module_i2c_driver(ina233_driver);

MODULE_AUTHOR("Leo Yang <Leo-Yang@quantatw.com>");
MODULE_DESCRIPTION("PMBus driver for INA233 and compatible chips");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("PMBUS");
