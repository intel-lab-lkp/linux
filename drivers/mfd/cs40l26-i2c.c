// SPDX-License-Identifier: GPL-2.0
/*
 * CS40L26 Boosted Haptic Driver with Integrated DSP and
 * Waveform Memory with Advanced Closed Loop Algorithms and LRA protection
 *
 * Copyright 2025 Cirrus Logic, Inc.
 *
 * Author: Fred Treven <ftreven@opensource.cirrus.com>
 */

#include <linux/i2c.h>
#include <linux/mfd/cs40l26.h>

static int cs40l26_i2c_probe(struct i2c_client *i2c)
{
	struct cs40l26 *cs40l26;

	cs40l26 = devm_kzalloc(&i2c->dev, sizeof(struct cs40l26), GFP_KERNEL);
	if (!cs40l26)
		return -ENOMEM;

	i2c_set_clientdata(i2c, cs40l26);

	cs40l26->dev = &i2c->dev;
	cs40l26->irq = i2c->irq;
	cs40l26->bus = &i2c_bus_type;

	cs40l26->regmap = devm_regmap_init_i2c(i2c, &cs40l26_regmap);
	if (IS_ERR(cs40l26->regmap))
		return dev_err_probe(cs40l26->dev, PTR_ERR(cs40l26->regmap),
				     "Failed to allocate register map\n");

	return cs40l26_probe(cs40l26);
}

static const struct i2c_device_id cs40l26_id_i2c[] = {
	{ "cs40l26a", 0 },
	{ "cs40l27b", 1 },
	{}
};
MODULE_DEVICE_TABLE(i2c, cs40l26_id_i2c);

static const struct of_device_id cs40l26_of_match[] = {
	{ .compatible = "cirrus,cs40l26a" },
	{ .compatible = "cirrus,cs40l27b" },
	{}
};
MODULE_DEVICE_TABLE(of, cs40l26_of_match);

static struct i2c_driver cs40l26_i2c_driver = {
	.driver = {
		.name = "cs40l26",
		.of_match_table = cs40l26_of_match,
		.pm = pm_ptr(&cs40l26_pm_ops),
	},
	.id_table = cs40l26_id_i2c,
	.probe = cs40l26_i2c_probe,
};
module_i2c_driver(cs40l26_i2c_driver);

MODULE_DESCRIPTION("CS40L26 I2C Driver");
MODULE_AUTHOR("Fred Treven, Cirrus Logic Inc. <ftreven@opensource.cirrus.com>");
MODULE_LICENSE("GPL");
