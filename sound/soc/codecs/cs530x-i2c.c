// SPDX-License-Identifier: GPL-2.0
//
// CS530x CODEC driver
//
// Copyright (C) 2024 Cirrus Logic, Inc. and
//                    Cirrus Logic International Semiconductor Ltd.

#include <linux/device.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>

#include "cs530x.h"

static const struct of_device_id cs530x_of_match[] = {
	{
		.compatible = "cirrus,cs5302",
		.data = (void *)CS5302,
	}, {
		.compatible = "cirrus,cs5304",
		.data = (void *)CS5304,
	}, {
		.compatible = "cirrus,cs5308",
		.data = (void *)CS5308,
	},
	{}
};
MODULE_DEVICE_TABLE(of, cs530x_of_match);

static const struct i2c_device_id cs530x_i2c_id[] = {
	{ "cs5302", CS5302 },
	{ "cs5304", CS5304 },
	{ "cs5308", CS5308 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cs530x_i2c_id);

static int cs530x_i2c_probe(struct i2c_client *client)
{
	struct cs530x_priv *cs530x;
	int ret;

	cs530x = devm_kzalloc(&client->dev, sizeof(struct cs530x_priv),
			       GFP_KERNEL);
	if (!cs530x)
		return -ENOMEM;

	i2c_set_clientdata(client, cs530x);

	cs530x->regmap = devm_regmap_init_i2c(client, &cs530x_regmap);
	if (IS_ERR(cs530x->regmap)) {
		ret = PTR_ERR(cs530x->regmap);
		dev_err(&client->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	if (client->dev.of_node) {
		const struct of_device_id *match;

		match = of_match_node(cs530x_of_match, client->dev.of_node);
		if (match == NULL)
			return -EINVAL;
		cs530x->devtype = (enum cs530x_type)match->data;
	} else {
		const struct i2c_device_id *id =
			i2c_match_id(cs530x_i2c_id, client);

		cs530x->devtype = id->driver_data;
	}

	cs530x->dev = &client->dev;

	return cs530x_probe(cs530x);
}

static struct i2c_driver cs530x_i2c_driver = {
	.driver = {
		.name = "cs530x",
		.of_match_table = cs530x_of_match,
	},
	.probe = cs530x_i2c_probe,
	.id_table = cs530x_i2c_id,
};
module_i2c_driver(cs530x_i2c_driver);

MODULE_DESCRIPTION("I2C CS530X driver");
MODULE_AUTHOR("Paul Handrigan, Cirrus Logic Inc, <paulha@opensource.cirrus.com>");
MODULE_IMPORT_NS(SND_SOC_CS530X);
