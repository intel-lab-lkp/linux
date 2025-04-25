// SPDX-License-Identifier: GPL-2.0-only

#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/mfd/zl3073x.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include "zl3073x.h"

static int zl3073x_spi_probe(struct spi_device *spi)
{
	struct regmap_config regmap_cfg;
	struct device *dev = &spi->dev;
	struct zl3073x_dev *zldev;

	zldev = zl3073x_devm_alloc(dev);
	if (IS_ERR(zldev))
		return PTR_ERR(zldev);

	zl3073x_dev_init_regmap_config(&regmap_cfg);

	zldev->regmap = devm_regmap_init_spi(spi, &regmap_cfg);
	if (IS_ERR(zldev->regmap)) {
		dev_err_probe(dev, PTR_ERR(zldev->regmap),
			      "Failed to initialize regmap\n");
		return PTR_ERR(zldev->regmap);
	}

	/* Initialize device and use SPI chip select value as dev ID */
	return zl3073x_dev_probe(zldev, spi_get_device_match_data(spi),
				 spi_get_chipselect(spi, 0));
}

static const struct spi_device_id zl3073x_spi_id[] = {
	{ "zl30731", .driver_data = (kernel_ulong_t)&zl3073x_chip_info[ZL30731] },
	{ "zl30731", .driver_data = (kernel_ulong_t)&zl3073x_chip_info[ZL30732] },
	{ "zl30731", .driver_data = (kernel_ulong_t)&zl3073x_chip_info[ZL30733] },
	{ "zl30731", .driver_data = (kernel_ulong_t)&zl3073x_chip_info[ZL30734] },
	{ "zl30731", .driver_data = (kernel_ulong_t)&zl3073x_chip_info[ZL30735] },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(spi, zl3073x_spi_id);

static const struct of_device_id zl3073x_spi_of_match[] = {
	{ .compatible = "microchip,zl30731", .data = &zl3073x_chip_info[ZL30731] },
	{ .compatible = "microchip,zl30732", .data = &zl3073x_chip_info[ZL30732] },
	{ .compatible = "microchip,zl30733", .data = &zl3073x_chip_info[ZL30733] },
	{ .compatible = "microchip,zl30734", .data = &zl3073x_chip_info[ZL30734] },
	{ .compatible = "microchip,zl30735", .data = &zl3073x_chip_info[ZL30735] },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, zl3073x_spi_of_match);

static struct spi_driver zl3073x_spi_driver = {
	.driver = {
		.name = "zl3073x-spi",
		.of_match_table = zl3073x_spi_of_match,
	},
	.probe = zl3073x_spi_probe,
	.id_table = zl3073x_spi_id,
};
module_spi_driver(zl3073x_spi_driver);

MODULE_AUTHOR("Ivan Vecera <ivecera@redhat.com>");
MODULE_DESCRIPTION("Microchip ZL3073x SPI driver");
MODULE_IMPORT_NS("ZL3073X");
MODULE_LICENSE("GPL");
