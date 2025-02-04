// SPDX-License-Identifier: GPL-2.0
/*
 * CS40L26 Boosted Haptic Driver with Integrated DSP and
 * Waveform Memory with Advanced Closed Loop Algorithms and LRA protection
 *
 * Copyright 2025 Cirrus Logic, Inc.
 *
 * Author: Fred Treven <ftreven@opensource.cirrus.com>
 */

#include <linux/mfd/cs40l26.h>
#include <linux/spi/spi.h>

static int cs40l26_spi_probe(struct spi_device *spi)
{
	struct cs40l26 *cs40l26;

	cs40l26 = devm_kzalloc(&spi->dev, sizeof(struct cs40l26), GFP_KERNEL);
	if (!cs40l26)
		return -ENOMEM;

	spi_set_drvdata(spi, cs40l26);

	cs40l26->dev = &spi->dev;
	cs40l26->irq = spi->irq;
	cs40l26->bus = &spi_bus_type;

	cs40l26->regmap = devm_regmap_init_spi(spi, &cs40l26_regmap);
	if (IS_ERR(cs40l26->regmap))
		return dev_err_probe(cs40l26->dev, PTR_ERR(cs40l26->regmap),
				     "Failed to allocate register map\n");

	return cs40l26_probe(cs40l26);
}

static const struct spi_device_id cs40l26_id_spi[] = {
	{ "cs40l26a", 0 },
	{ "cs40l27b", 1 },
	{}
};
MODULE_DEVICE_TABLE(spi, cs40l26_id_spi);

static const struct of_device_id cs40l26_of_match[] = {
	{ .compatible = "cirrus,cs40l26a" },
	{ .compatible = "cirrus,cs40l27b" },
	{}
};
MODULE_DEVICE_TABLE(of, cs40l26_of_match);

static struct spi_driver cs40l26_spi_driver = {
	.driver = {
		.name = "cs40l26",
		.of_match_table = cs40l26_of_match,
		.pm = pm_ptr(&cs40l26_pm_ops),
	},
	.id_table = cs40l26_id_spi,
	.probe = cs40l26_spi_probe,
};
module_spi_driver(cs40l26_spi_driver);

MODULE_DESCRIPTION("CS40L26 SPI Driver");
MODULE_AUTHOR("Fred Treven, Cirrus Logic Inc. <ftreven@opensource.cirrus.com>");
MODULE_LICENSE("GPL");
