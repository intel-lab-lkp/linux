// SPDX-License-Identifier: GPL-2.0
/*
 * ScioSense ENS160 multi-gas sensor I2C driver
 *
 * Copyright (c) 2024 Gustavo Silva <gustavograzs@gmail.com>
 *
 * 7-Bit I2C slave address is:
 *	- 0x52 if ADDR pin LOW
 *	- 0x53 if ADDR pin HIGH
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include "ens160.h"

static const struct regmap_config ens160_regmap_i2c_conf = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int ens160_i2c_probe(struct i2c_client *client)
{
	struct regmap *regmap;

	regmap = devm_regmap_init_i2c(client, &ens160_regmap_i2c_conf);
	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "Failed to register i2c regmap %ld\n",
			PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}

	return ens160_core_probe(&client->dev, regmap, client->name);
}

static void ens160_i2c_remove(struct i2c_client *client)
{
	ens160_core_remove(&client->dev);
}

static const struct i2c_device_id ens160_i2c_id[] = {
	{ "ens160", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ens160_i2c_id);

static const struct of_device_id ens160_of_i2c_match[] = {
	{ .compatible = "sciosense,ens160" },
	{ }
};
MODULE_DEVICE_TABLE(of, ens160_of_i2c_match);

static struct i2c_driver ens160_i2c_driver = {
	.driver = {
		.name		= "ens160_i2c",
		.of_match_table	= ens160_of_i2c_match,
	},
	.probe = ens160_i2c_probe,
	.remove = ens160_i2c_remove,
	.id_table = ens160_i2c_id,
};
module_i2c_driver(ens160_i2c_driver);

MODULE_AUTHOR("Gustavo Silva <gustavograzs@gmail.com>");
MODULE_DESCRIPTION("ScioSense ENS160 I2C driver");
MODULE_LICENSE("GPL v2");
MODULE_IMPORT_NS(IIO_ENS160);
