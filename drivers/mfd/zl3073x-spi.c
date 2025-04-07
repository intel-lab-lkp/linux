// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include "zl3073x.h"

static const struct spi_device_id zl3073x_spi_id[] = {
	{ "zl3073x-spi", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(spi, zl3073x_spi_id);

static const struct of_device_id zl3073x_spi_of_match[] = {
	{ .compatible = "microchip,zl3073x-spi" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, zl3073x_spi_of_match);

static int zl3073x_spi_probe(struct spi_device *spidev)
{
	struct device *dev = &spidev->dev;
	const struct spi_device_id *id;
	struct zl3073x_dev *zldev;
	int rc;

	zldev = zl3073x_dev_alloc(dev);
	if (!zldev)
		return -ENOMEM;

	id = spi_get_device_id(spidev);
	zldev->dev = dev;

	zldev->regmap = devm_regmap_init_spi(spidev,
					     zl3073x_get_regmap_config());
	if (IS_ERR(zldev->regmap)) {
		rc = PTR_ERR(zldev->regmap);
		dev_err(dev, "Failed to allocate register map: %d\n", rc);
		return rc;
	}

	spi_set_drvdata(spidev, zldev);

	return zl3073x_dev_init(zldev);
}

static void zl3073x_spi_remove(struct spi_device *spidev)
{
	struct zl3073x_dev *zldev;

	zldev = spi_get_drvdata(spidev);
	zl3073x_dev_exit(zldev);
}

static struct spi_driver zl3073x_spi_driver = {
	.driver = {
		.name = "zl3073x-spi",
		.of_match_table = of_match_ptr(zl3073x_spi_of_match),
	},
	.probe = zl3073x_spi_probe,
	.remove = zl3073x_spi_remove,
	.id_table = zl3073x_spi_id,
};

module_spi_driver(zl3073x_spi_driver);

MODULE_AUTHOR("Ivan Vecera <ivecera@redhat.com>");
MODULE_DESCRIPTION("Microchip ZL3073x SPI driver");
MODULE_IMPORT_NS("ZL3073X");
MODULE_LICENSE("GPL");
