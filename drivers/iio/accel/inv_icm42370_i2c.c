// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 InvenSense, Inc.
 * Copyright (C) 2026 Axis Communications AB
 */

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include "inv_icm42370.h"

/**
 * inv_icm42370_i2c_bus_setup() - I2C bus setup for icm42370
 *
 * @data: pointer to struct containing the sensor data
 *
 * Returns 0 on success, negative errno on error
 */
static int inv_icm42370_i2c_bus_setup(struct inv_icm42370_data *data)
{
	unsigned int mask, val;
	int ret;

	/* set slew rates for I2C */
	mask = INV_ICM42370_DRIVE_CONFIG2_I2C_MASK;
	val = INV_ICM42370_DRIVE_CONFIG2_I2C(INV_ICM42370_SLEW_RATE_12_36NS);
	ret = regmap_update_bits(data->map, INV_ICM42370_REG_DRIVE_CONFIG2,
				 mask, val);
	if (ret)
		return ret;

	/* set slew rates for SPI */
	mask = INV_ICM42370_DRIVE_CONFIG3_SPI_MASK;
	val = INV_ICM42370_DRIVE_CONFIG3_SPI(INV_ICM42370_SLEW_RATE_12_36NS);
	ret = regmap_update_bits(data->map, INV_ICM42370_REG_DRIVE_CONFIG3,
				 mask, val);
	if (ret)
		return ret;

	return 0;
}
static int inv_icm42370_probe(struct i2c_client *client)
{
	const void *match;
	enum inv_icm42370_chip chip;
	struct regmap *regmap;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_I2C_BLOCK))
		return -EOPNOTSUPP;

	match = device_get_match_data(&client->dev);
	if (!match)
		return -EINVAL;
	chip = (uintptr_t)match;

	regmap = devm_regmap_init_i2c(client, &inv_icm42370_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return inv_icm42370_core_probe(regmap, chip, client->irq,
				       inv_icm42370_i2c_bus_setup);
}

static const struct i2c_device_id inv_icm42370_id[] = {
	{ .name = "icm42370p", .driver_data = INV_CHIP_ICM42370 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, inv_icm42370_id);

static const struct of_device_id inv_icm42370_of_matches[] = {
	{ .compatible = "invensense,icm42370p", .data = (void *)INV_CHIP_ICM42370 },
	{ }
};
MODULE_DEVICE_TABLE(of, inv_icm42370_of_matches);

static struct i2c_driver inv_icm42370_driver = {
	.driver = {
		.name = "inv-icm42370-i2c",
		.of_match_table = inv_icm42370_of_matches,
	},
	.probe = inv_icm42370_probe,
	.id_table = inv_icm42370_id,
};
module_i2c_driver(inv_icm42370_driver);

MODULE_AUTHOR("Kanak Shilledar <kanak.shilledar@axis.com>");
MODULE_AUTHOR("Henrik Grimler <henrik.grimler@axis.com>");
MODULE_DESCRIPTION("InvenSense ICM-42370-P I2C driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_ICM42370");
