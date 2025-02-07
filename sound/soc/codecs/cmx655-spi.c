// SPDX-License-Identifier: GPL-2.0-only

#include <linux/version.h>
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/of.h>

#include "cmx655.h"

static int cmx655_spi_probe(struct spi_device *spi)
{
	int ret;
	struct regmap *regmap = devm_regmap_init_spi(spi, &cmx655_regmap);

	ret =
	    cmx655_common_register_component(&spi->dev,
					     regmap,
					     spi->irq);
	if (ret < 0) {
		dev_err(&spi->dev,
			"%s: Register component failed %d\n", __func__, ret);
	}

	return ret;
};

static void cmx655_spi_remove(struct spi_device *spi)
{
	cmx655_common_unregister_component(&spi->dev);
};

static const struct spi_device_id cmx655_device_id[] = {
	{ "cmx655" },
	{ }
};

MODULE_DEVICE_TABLE(spi, cmx655_device_id);

static const struct of_device_id cmx655_of_match[] = {
	{.compatible = "cml,cmx655d" },
	{ }
};

MODULE_DEVICE_TABLE(of, cmx655_of_match);

static struct spi_driver cmx655_spi_driver = {
	.probe = cmx655_spi_probe,
	.remove = cmx655_spi_remove,
	.driver = {
		   .name = "cmx655",
		   .of_match_table = cmx655_of_match,
		    },
	.id_table = cmx655_device_id
};

module_spi_driver(cmx655_spi_driver);

MODULE_DESCRIPTION("ASoC CMX655 driver, SPI adapter");
MODULE_AUTHOR("CML");
MODULE_LICENSE("GPL");
