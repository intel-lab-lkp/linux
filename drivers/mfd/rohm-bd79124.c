// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (C) 2025 ROHM Semiconductors
//
// ROHM BD79124 ADC / GPO driver

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <linux/mfd/core.h>
#include <linux/mfd/rohm-bd79124.h>

static struct resource adc_alert;

enum {
	CELL_PINMUX,
	CELL_ADC,
};

static struct mfd_cell bd79124_cells[] = {
	[CELL_PINMUX]	= { .name = "bd79124-pinmux", },
	[CELL_ADC]	= { .name = "bd79124-adc", },
};

/* Read-only regs */
static const struct regmap_range bd79124_ro_ranges[] = {
	{
		.range_min = BD79124_REG_EVENT_FLAG,
		.range_max = BD79124_REG_EVENT_FLAG,
	}, {
		.range_min = BD79124_REG_RECENT_CH0_LSB,
		.range_max = BD79124_REG_RECENT_CH7_MSB,
	},
};

static const struct regmap_access_table bd79124_ro_regs = {
	.no_ranges	= &bd79124_ro_ranges[0],
	.n_no_ranges	= ARRAY_SIZE(bd79124_ro_ranges),
};

static const struct regmap_range bd79124_volatile_ranges[] = {
	{
		.range_min = BD79124_REG_RECENT_CH0_LSB,
		.range_max = BD79124_REG_RECENT_CH7_MSB,
	}, {
		.range_min = BD79124_REG_EVENT_FLAG,
		.range_max = BD79124_REG_EVENT_FLAG,
	}, {
		.range_min = BD79124_REG_EVENT_FLAG_HI,
		.range_max = BD79124_REG_EVENT_FLAG_HI,
	}, {
		.range_min = BD79124_REG_EVENT_FLAG_LO,
		.range_max = BD79124_REG_EVENT_FLAG_LO,
	}, {
		.range_min = BD79124_REG_SYSTEM_STATUS,
		.range_max = BD79124_REG_SYSTEM_STATUS,
	},
};

static const struct regmap_access_table bd79124_volatile_regs = {
	.yes_ranges	= &bd79124_volatile_ranges[0],
	.n_yes_ranges	= ARRAY_SIZE(bd79124_volatile_ranges),
};

static const struct regmap_range bd79124_precious_ranges[] = {
	{
		.range_min = BD79124_REG_EVENT_FLAG_HI,
		.range_max = BD79124_REG_EVENT_FLAG_HI,
	}, {
		.range_min = BD79124_REG_EVENT_FLAG_LO,
		.range_max = BD79124_REG_EVENT_FLAG_LO,
	},
};

static const struct regmap_access_table bd79124_precious_regs = {
	.yes_ranges	= &bd79124_precious_ranges[0],
	.n_yes_ranges	= ARRAY_SIZE(bd79124_precious_ranges),
};

static const struct regmap_config bd79124_regmap = {
	.reg_bits		= 16,
	.val_bits		= 8,
	.read_flag_mask		= BD79124_I2C_MULTI_READ,
	.write_flag_mask	= BD79124_I2C_MULTI_WRITE,
	.max_register		= BD79124_REG_MAX,
	.cache_type		= REGCACHE_MAPLE,
	.volatile_table		= &bd79124_volatile_regs,
	.wr_table		= &bd79124_ro_regs,
	.precious_table		= &bd79124_precious_regs,
};

static int bd79124_probe(struct i2c_client *i2c)
{
	int ret;
	struct regmap *map;
	struct device *dev = &i2c->dev;
	int *adc_vref;

	adc_vref = devm_kzalloc(dev, sizeof(*adc_vref), GFP_KERNEL);
	if (!adc_vref)
		return -ENOMEM;

	/*
	 * Better to enable regulators here so we don't need to worry about the
	 * order of sub-device instantiation. We also need to deliver the
	 * reference voltage value to the ADC driver. This is done via
	 * the MFD driver's drvdata.
	 */
	*adc_vref = devm_regulator_get_enable_read_voltage(dev, "vdd");
	if (*adc_vref < 0)
		return dev_err_probe(dev, ret, "Failed to get the Vdd\n");

	dev_set_drvdata(dev, adc_vref);

	ret = devm_regulator_get_enable(dev, "iovdd");
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to enable I/O voltage\n");

	map = devm_regmap_init_i2c(i2c, &bd79124_regmap);
	if (IS_ERR(map))
		return dev_err_probe(dev, PTR_ERR(map),
				     "Failed to initialize Regmap\n");

	if (i2c->irq) {
		adc_alert = DEFINE_RES_IRQ_NAMED(i2c->irq, "thresh-alert");
		bd79124_cells[CELL_ADC].resources = &adc_alert;
		bd79124_cells[CELL_ADC].num_resources = 1;
	}

	ret = devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, bd79124_cells,
				   ARRAY_SIZE(bd79124_cells), NULL, 0, NULL);
	if (ret)
		dev_err_probe(dev, ret, "Failed to create subdevices\n");

	return ret;
}

static const struct of_device_id bd79124_of_match[] = {
	{ .compatible = "rohm,bd79124" },
	{ }
};
MODULE_DEVICE_TABLE(of, bd79124_of_match);

static const struct i2c_device_id bd79124_id[] = {
	{ "bd79124", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bd79124_id);

static struct i2c_driver bd79124_driver = {
	.driver = {
		.name = "bd79124",
		.of_match_table = bd79124_of_match,
	},
	.probe = bd79124_probe,
	.id_table = bd79124_id,
};
module_i2c_driver(bd79124_driver);

MODULE_AUTHOR("Matti Vaittinen <mazziesaccount@gmail.com>");
MODULE_DESCRIPTION("Core Driver for ROHM BD79124");
MODULE_LICENSE("GPL");
