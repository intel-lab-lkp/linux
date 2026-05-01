// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 InvenSense, Inc.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/property.h>

#include "inv_icm42607.h"

static int inv_icm42607_i2c_bus_setup(struct inv_icm42607_state *st)
{
	unsigned int val;
	int ret;

	ret = regmap_clear_bits(st->map, INV_ICM42607_REG_INTF_CONFIG1,
				INV_ICM42607_INTF_CONFIG1_I3C_DDR_EN |
				INV_ICM42607_INTF_CONFIG1_I3C_SDR_EN);
	if (ret)
		return ret;

	val = FIELD_PREP(INV_ICM42607_DRIVE_CONFIG2_I2C_MASK,
			 INV_ICM42607_SLEW_RATE_12_36NS);
	ret = regmap_update_bits(st->map, INV_ICM42607_REG_DRIVE_CONFIG2,
				 INV_ICM42607_DRIVE_CONFIG2_I2C_MASK, val);
	if (ret)
		return ret;

	return regmap_update_bits(st->map, INV_ICM42607_REG_INTF_CONFIG0,
				  INV_ICM42607_INTF_CONFIG0_UI_SIFS_CFG_MASK,
				  INV_ICM42607_INTF_CONFIG0_UI_SIFS_CFG_SPI_DIS);
}

static int inv_icm42607_probe(struct i2c_client *client)
{
	const void *match;
	enum inv_icm42607_chip chip;
	struct regmap *regmap;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_I2C_BLOCK))
		return -EOPNOTSUPP;

	match = i2c_get_match_data(client);
	chip = (kernel_ulong_t)match;

	regmap = devm_regmap_init_i2c(client, &inv_icm42607_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return inv_icm42607_core_probe(regmap, chip, inv_icm42607_i2c_bus_setup);
}

static const struct i2c_device_id inv_icm42607_id[] = {
	{ "icm42607", INV_CHIP_ICM42607 },
	{ "icm42607p", INV_CHIP_ICM42607P },
	{ }
};
MODULE_DEVICE_TABLE(i2c, inv_icm42607_id);

static const struct of_device_id inv_icm42607_of_matches[] = {
	{
		.compatible = "invensense,icm42607",
		.data = (void *)INV_CHIP_ICM42607,
	}, {
		.compatible = "invensense,icm42607p",
		.data = (void *)INV_CHIP_ICM42607P,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, inv_icm42607_of_matches);

static struct i2c_driver inv_icm42607_driver = {
	.driver = {
		.name = "inv-icm42607-i2c",
		.of_match_table = inv_icm42607_of_matches,
	},
	.id_table = inv_icm42607_id,
	.probe = inv_icm42607_probe,
};
module_i2c_driver(inv_icm42607_driver);

MODULE_AUTHOR("InvenSense, Inc.");
MODULE_DESCRIPTION("InvenSense ICM-42607x I2C driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_ICM42607");
