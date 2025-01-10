// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hardware monitoring driver for ina233
 *
 * Copyright (c) 2025 Leo Yang
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include "pmbus.h"

#define MFR_READ_VSHUNT 0xd1
#define MFR_CALIBRATION 0xd4

#define INA233_CURRENT_LSB_DEFAULT	1000 /* uA */
#define INA233_RSHUNT_DEFAULT		2000 /* uOhm */

#define MAX_M_VAL 32767
#define MIN_M_VAL -32768

static void calculate_coef(int *m, int *R, int power_coef)
{
	s64 scaled_m;
	int scale_factor = 0;
	int scale_coef = 1;
	bool is_integer = false;

	/*
	 * 1000000 from Current_LSB A->uA .
	 * scale_coef is for scaling up to minimize rounding errors,
	 * If there is no decimal information, No need to scale.
	 */
	if (1000000 % *m) {
		/* Scaling to keep integer precision */
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
		scaled_m = div_s64(scaled_m, 10);
		scale_factor++;
	}
	/* Scale up only if fractional part exists. */
	while (scaled_m * 10 < MAX_M_VAL && scaled_m * 10 > MIN_M_VAL && !is_integer) {
		scaled_m *= 10;
		scale_factor--;
	}

	*m = scaled_m;
	*R = scale_factor;
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

static int ina233_probe(struct i2c_client *client)
{
	int ret, m, R;
	u32 rshunt;
	u16 current_lsb;
	u16 calibration;
	struct pmbus_driver_info *info;

	info = devm_kzalloc(&client->dev, sizeof(struct pmbus_driver_info),
			    GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->pages = 1;
	info->format[PSC_VOLTAGE_IN] = direct;
	info->format[PSC_VOLTAGE_OUT] = direct;
	info->format[PSC_CURRENT_OUT] = direct;
	info->format[PSC_POWER] = direct;
	info->func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_INPUT
		| PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT
		| PMBUS_HAVE_POUT
		| PMBUS_HAVE_VMON | PMBUS_HAVE_STATUS_VMON;
	info->m[PSC_VOLTAGE_IN] = 8;
	info->R[PSC_VOLTAGE_IN] = 2;
	info->m[PSC_VOLTAGE_OUT] = 8;
	info->R[PSC_VOLTAGE_OUT] = 2;
	info->read_word_data = ina233_read_word_data;

	/* If INA233 skips current/power, shunt-resistor and current-lsb aren't needed.	*/
	/* read rshunt value (uOhm) */
	if (of_property_read_u32(client->dev.of_node, "shunt-resistor", &rshunt) < 0)
		rshunt = INA233_RSHUNT_DEFAULT;

	/* read current_lsb value (uA) */
	if (of_property_read_u16(client->dev.of_node, "ti,current-lsb", &current_lsb) < 0)
		current_lsb = INA233_CURRENT_LSB_DEFAULT;

	if (!rshunt || !current_lsb) {
		dev_err(&client->dev, "shunt-resistor and current-lsb cannot be zero.\n");
		return -EINVAL;
	}

	/* calculate current coefficient */
	m = current_lsb;
	calculate_coef(&m, &R, 1);
	info->m[PSC_CURRENT_OUT] = m;
	info->R[PSC_CURRENT_OUT] = R;

	/* calculate power coefficient */
	m = current_lsb;
	calculate_coef(&m, &R, 25);
	info->m[PSC_POWER] = m;
	info->R[PSC_POWER] = R;

	/* write MFR_CALIBRATION register, Apply formula from spec with unit scaling. */
	calibration = div64_u64(5120000000ULL, (u64)rshunt * current_lsb);
	if (calibration > 0x7FFF) {
		dev_err(&client->dev, "Exceeds MFR_CALIBRATION limit, Use a reasonable Current_LSB with Shunt resistor value.\n");
		return -EINVAL;
	}
	ret = i2c_smbus_write_word_data(client, MFR_CALIBRATION, calibration);
	if (ret < 0) {
		dev_err(&client->dev, "Unable to write calibration\n");
		return ret;
	}

	dev_dbg(&client->dev, "power monitor %s (Rshunt = %u uOhm, Current_LSB = %u uA/bit)\n",
		client->name, rshunt, current_lsb);

	return pmbus_do_probe(client, info);
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
