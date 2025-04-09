// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/mfd/zl3073x.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include "zl3073x.h"

static int zl3073x_spi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct zl3073x_dev *zldev;

	zldev = zl3073x_devm_alloc(dev);
	if (!zldev)
		return -ENOMEM;

	zldev->dev = dev;
	zldev->regmap = devm_regmap_init_spi(spi, zl3073x_get_regmap_config());
	if (IS_ERR(zldev->regmap)) {
		dev_err_probe(dev, PTR_ERR(zldev->regmap),
			      "Failed to initialize register map\n");
		return PTR_ERR(zldev->regmap);
	}

	spi_set_drvdata(spi, zldev);

	return zl3073x_dev_init(zldev);
}

static const struct spi_device_id zl3073x_spi_id[] = {
	{ "zl3073x-spi" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(spi, zl3073x_spi_id);

static const struct of_device_id zl3073x_spi_of_match[] = {
	{ .compatible = "microchip,zl3073x-spi" },
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
