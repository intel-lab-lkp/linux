// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 InvenSense, Inc.
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/property.h>

#include "inv_icm42607.h"

static int inv_icm42607_spi_bus_setup(struct inv_icm42607_state *st)
{
	unsigned int val;
	int ret;

	ret = regmap_set_bits(st->map, INV_ICM42607_REG_DEVICE_CONFIG,
			      INV_ICM42607_DEVICE_CONFIG_SPI_AP_4WIRE);
	if (ret)
		return ret;

	ret = regmap_clear_bits(st->map, INV_ICM42607_REG_INTF_CONFIG1,
				INV_ICM42607_INTF_CONFIG1_I3C_DDR_EN |
				INV_ICM42607_INTF_CONFIG1_I3C_SDR_EN);
	if (ret)
		return ret;

	val = FIELD_PREP(INV_ICM42607_DRIVE_CONFIG3_SPI_MASK,
			 INV_ICM42607_SLEW_RATE_INF_2NS);
	ret = regmap_update_bits(st->map, INV_ICM42607_REG_DRIVE_CONFIG3,
				 INV_ICM42607_DRIVE_CONFIG3_SPI_MASK, val);
	if (ret)
		return ret;

	return regmap_update_bits(st->map, INV_ICM42607_REG_INTF_CONFIG0,
				  INV_ICM42607_INTF_CONFIG0_UI_SIFS_CFG_MASK,
				  INV_ICM42607_INTF_CONFIG0_UI_SIFS_CFG_I2C_DIS);
}

static int inv_icm42607_probe(struct spi_device *spi)
{
	const void *match;
	enum inv_icm42607_chip chip;
	struct regmap *regmap;

	match = spi_get_device_match_data(spi);
	chip = (kernel_ulong_t)match;

	regmap = devm_regmap_init_spi(spi, &inv_icm42607_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&spi->dev, PTR_ERR(regmap),
				     "Failed to register spi regmap %ld\n",
				     PTR_ERR(regmap));

	return inv_icm42607_core_probe(regmap, chip,
				       inv_icm42607_spi_bus_setup);
}

static const struct of_device_id inv_icm42607_of_matches[] = {
	{
		.compatible = "invensense,icm42607",
		.data = (void *)INV_CHIP_ICM42607,
	},
	{
		.compatible = "invensense,icm42607p",
		.data = (void *)INV_CHIP_ICM42607P,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, inv_icm42607_of_matches);

static const struct spi_device_id inv_icm42607_spi_id_table[] = {
	{ "icm42607", INV_CHIP_ICM42607 },
	{ "icm42607p", INV_CHIP_ICM42607P },
	{ }
};
MODULE_DEVICE_TABLE(spi, inv_icm42607_spi_id_table);

static struct spi_driver inv_icm42607_driver = {
	.driver = {
		.name = "inv-icm42607-spi",
		.of_match_table = inv_icm42607_of_matches,
		.pm = &inv_icm42607_pm_ops,
	},
	.id_table = inv_icm42607_spi_id_table,
	.probe = inv_icm42607_probe,
};
module_spi_driver(inv_icm42607_driver);

MODULE_AUTHOR("InvenSense, Inc.");
MODULE_DESCRIPTION("InvenSense ICM-42607x SPI driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_ICM42607");
