// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 InvenSense, Inc.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>

#include "inv_icm45600.h"

static const struct regmap_config inv_icm45600_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int inv_icm45600_spi_bus_setup(struct inv_icm45600_state *st)
{
	/* set slew rates for SPI */
	return regmap_update_bits(st->map, INV_ICM45600_REG_DRIVE_CONFIG0,
				INV_ICM45600_DRIVE_CONFIG0_SPI_MASK,
				INV_ICM45600_SPI_SLEW_RATE_5NS);
}

static int inv_icm45600_probe(struct spi_device *spi)
{
	const void *match;
	enum inv_icm45600_chip chip;
	struct regmap *regmap;

	match = device_get_match_data(&spi->dev);
	if (!match)
		return -EINVAL;
	chip = (uintptr_t)match;

	/* use SPI specific regmap */
	regmap = devm_regmap_init_spi(spi, &inv_icm45600_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return inv_icm45600_core_probe(regmap, chip, true,
				       inv_icm45600_spi_bus_setup);
}

/*
 * device id table is used to identify what device can be
 * supported by this driver
 */
static const struct spi_device_id inv_icm45600_id[] = {
	{ "icm45605", INV_CHIP_ICM45605 },
	{ "icm45686", INV_CHIP_ICM45686 },
	{ "icm45688p", INV_CHIP_ICM45688P },
	{ "icm45608", INV_CHIP_ICM45608 },
	{ "icm45634", INV_CHIP_ICM45634 },
	{ "icm45689", INV_CHIP_ICM45689 },
	{ "icm45606", INV_CHIP_ICM45606 },
	{ "icm45687", INV_CHIP_ICM45687 },
	{ }
};
MODULE_DEVICE_TABLE(spi, inv_icm45600_id);

static const struct of_device_id inv_icm45600_of_matches[] = {
	{
		.compatible = "invensense,icm45605",
		.data = (void *)INV_CHIP_ICM45605,
	}, {
		.compatible = "invensense,icm45686",
		.data = (void *)INV_CHIP_ICM45686,
	}, {
		.compatible = "invensense,icm45688p",
		.data = (void *)INV_CHIP_ICM45688P,
	}, {
		.compatible = "invensense,icm45608",
		.data = (void *)INV_CHIP_ICM45608,
	}, {
		.compatible = "invensense,icm45634",
		.data = (void *)INV_CHIP_ICM45634,
	}, {
		.compatible = "invensense,icm45689",
		.data = (void *)INV_CHIP_ICM45689,
	}, {
		.compatible = "invensense,icm45606",
		.data = (void *)INV_CHIP_ICM45606,
	}, {
		.compatible = "invensense,icm45687",
		.data = (void *)INV_CHIP_ICM45687,
	},
	{}
};
MODULE_DEVICE_TABLE(of, inv_icm45600_of_matches);

static struct spi_driver inv_icm45600_driver = {
	.driver = {
		.name = "inv-icm45600-spi",
		.of_match_table = inv_icm45600_of_matches,
		.pm = pm_ptr(&inv_icm45600_pm_ops),
	},
	.id_table = inv_icm45600_id,
	.probe = inv_icm45600_probe,
};
module_spi_driver(inv_icm45600_driver);

MODULE_AUTHOR("InvenSense, Inc.");
MODULE_DESCRIPTION("InvenSense ICM-456xx SPI driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_ICM45600");
