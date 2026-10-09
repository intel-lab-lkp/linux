// SPDX-License-Identifier: GPL-2.0
/*
 * HID over SPI protocol, Open Firmware related code
 *
 * Copyright (c) 2021 Microsoft Corporation
 *
 * This code was forked out of the HID over SPI core code, which is partially
 * based on "HID over I2C protocol implementation:
 *
 * Copyright (c) 2012 Benjamin Tissoires <benjamin.tissoires@gmail.com>
 * Copyright (c) 2012 Ecole Nationale de l'Aviation Civile, France
 * Copyright (c) 2012 Red Hat, Inc
 *
 * which in turn is partially based on "USB HID support for Linux":
 *
 * Copyright (c) 1999 Andreas Gal
 * Copyright (c) 2000-2005 Vojtech Pavlik <vojtech@suse.cz>
 * Copyright (c) 2005 Michael Haboustak <mike-@cinci.rr.com> for Concept2, Inc
 * Copyright (c) 2007-2008 Oliver Neukum
 * Copyright (c) 2006-2010 Jiri Kosina
 */

#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/types.h>

#include "spi-hid.h"

struct spi_hid_timing_data {
	u32 post_power_on_delay_ms;
	u32 minimal_reset_delay_ms;
};

/* Config structure is filled with data from Device Tree */
struct spi_hid_of_config {
	struct spihid_ops ops;

	struct spi_hid_conf property_conf;
	const struct spi_hid_timing_data *timing_data;

	struct gpio_desc *reset_gpio;
	struct regulator *supply;
	bool supply_enabled;
};

static const struct spi_hid_timing_data timing_data = {
	.post_power_on_delay_ms = 10,
	.minimal_reset_delay_ms = 100,
};

static int spi_hid_of_populate_config(struct spi_hid_of_config *conf,
				      struct device *dev)
{
	int error;
	u32 val;

	error = device_property_read_u32(dev, "input-report-header-address",
					 &val);
	if (error) {
		dev_err(dev, "Input report header address not provided\n");
		return -ENODEV;
	}
	conf->property_conf.input_report_header_address = val;

	error = device_property_read_u32(dev, "input-report-body-address", &val);
	if (error) {
		dev_err(dev, "Input report body address not provided\n");
		return -ENODEV;
	}
	conf->property_conf.input_report_body_address = val;

	error = device_property_read_u32(dev, "output-report-address", &val);
	if (error) {
		dev_err(dev, "Output report address not provided\n");
		return -ENODEV;
	}
	conf->property_conf.output_report_address = val;

	error = device_property_read_u8(dev, "read-opcode",
					&conf->property_conf.read_opcode);
	if (error) {
		dev_err(dev, "Read opcode not provided\n");
		return -ENODEV;
	}

	error = device_property_read_u8(dev, "write-opcode",
					&conf->property_conf.write_opcode);
	if (error) {
		dev_err(dev, "Write opcode not provided\n");
		return -ENODEV;
	}

	conf->supply = devm_regulator_get(dev, "vdd");
	if (IS_ERR(conf->supply))
		return dev_err_probe(dev, PTR_ERR(conf->supply),
				     "Failed to get regulator\n");
	conf->supply_enabled = false;

	conf->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(conf->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(conf->reset_gpio),
				     "Failed to get reset GPIO\n");

	return 0;
}

static int spi_hid_of_power_down(struct spihid_ops *ops)
{
	struct spi_hid_of_config *conf = container_of(ops,
						      struct spi_hid_of_config,
						      ops);
	int error;

	if (!conf->supply_enabled)
		return 0;

	error = regulator_disable(conf->supply);
	if (error == 0)
		conf->supply_enabled = false;

	return error;
}

static int spi_hid_of_power_up(struct spihid_ops *ops)
{
	struct spi_hid_of_config *conf = container_of(ops,
						      struct spi_hid_of_config,
						      ops);
	int error;

	if (conf->supply_enabled)
		return 0;

	error = regulator_enable(conf->supply);

	if (error == 0) {
		conf->supply_enabled = true;
		fsleep(1000 * conf->timing_data->post_power_on_delay_ms);
	}

	return error;
}

static int spi_hid_of_assert_reset(struct spihid_ops *ops)
{
	struct spi_hid_of_config *conf = container_of(ops,
						      struct spi_hid_of_config,
						      ops);

	return gpiod_set_value_cansleep(conf->reset_gpio, 1);
}

static int spi_hid_of_deassert_reset(struct spihid_ops *ops)
{
	struct spi_hid_of_config *conf = container_of(ops,
						      struct spi_hid_of_config,
						      ops);

	return gpiod_set_value_cansleep(conf->reset_gpio, 0);
}

static void spi_hid_of_sleep_minimal_reset_delay(struct spihid_ops *ops)
{
	struct spi_hid_of_config *conf = container_of(ops,
						      struct spi_hid_of_config,
						      ops);
	fsleep(1000 * conf->timing_data->minimal_reset_delay_ms);
}

static int spi_hid_of_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct spi_hid_of_config *config;
	int error;

	config = devm_kzalloc(dev, sizeof(struct spi_hid_of_config),
			      GFP_KERNEL);
	if (!config)
		return -ENOMEM;

	config->ops.power_up = spi_hid_of_power_up;
	config->ops.power_down = spi_hid_of_power_down;
	config->ops.assert_reset = spi_hid_of_assert_reset;
	config->ops.deassert_reset = spi_hid_of_deassert_reset;
	config->ops.sleep_minimal_reset_delay =
		spi_hid_of_sleep_minimal_reset_delay;

	config->timing_data = device_get_match_data(dev);
	if (!config->timing_data)
		config->timing_data = &timing_data;

	/*
	 * FIXME: multi-SPI not supported. Once it is, derive the
	 * HID-over-SPI flags from spi->mode.
	 */

	error = spi_hid_of_populate_config(config, dev);
	if (error)
		return dev_err_probe(dev, error, "Unable to populate config data\n");

	return spi_hid_core_probe(spi, &config->ops, &config->property_conf);
}

static const struct of_device_id spi_hid_of_match[] = {
	{ .compatible = "hid-over-spi", .data = &timing_data },
	{}
};
MODULE_DEVICE_TABLE(of, spi_hid_of_match);

static const struct spi_device_id spi_hid_of_id_table[] = {
	{ "hid", 0 },
	{ "hid-over-spi", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, spi_hid_of_id_table);

static struct spi_driver spi_hid_of_driver = {
	.driver = {
		.name	= "spi_hid_of",
		.owner	= THIS_MODULE,
		.of_match_table = spi_hid_of_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.dev_groups = spi_hid_groups,
	},
	.probe		= spi_hid_of_probe,
	.remove		= spi_hid_core_remove,
	.id_table	= spi_hid_of_id_table,
};

module_spi_driver(spi_hid_of_driver);

MODULE_DESCRIPTION("HID over SPI OF transport driver");
MODULE_AUTHOR("Dmitry Antipov <dmanti@microsoft.com>");
MODULE_LICENSE("GPL");
