// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP P3T175x Temperature Sensor Driver
 *
 * Copyright 2025 NXP
 */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "p3t1755.h"

static const struct regmap_config p3t1755_i2c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static const struct of_device_id p3t1755_i2c_of_match[] = {
	{ .compatible = "nxp,p3t1750dp", .data = &p3t1750_channels_info },
	{ .compatible = "nxp,p3t1755dp", .data = &p3t1755_channels_info },
	{ }
};
MODULE_DEVICE_TABLE(of, p3t1755_i2c_of_match);

static const struct i2c_device_id p3t1755_i2c_id_table[] = {
	{ "p3t1750", (kernel_ulong_t)&p3t1750_channels_info },
	{ "p3t1755", (kernel_ulong_t)&p3t1755_channels_info },
	{ }
};
MODULE_DEVICE_TABLE(i2c, p3t1755_i2c_id_table);

static int p3t1755_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	const struct p3t1755_info *chip;
	struct regmap *regmap;
	int ret;

	regmap = devm_regmap_init_i2c(client, &p3t1755_i2c_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "regmap init failed\n");

	chip = i2c_get_match_data(client);

	ret = p3t1755_probe(dev, chip, regmap, client->irq);
	if (ret)
		return dev_err_probe(dev, ret, "p3t175x probe failed: %d\n", ret);

	return 0;
}

static struct i2c_driver p3t1755_driver = {
	.driver = {
		.name = "p3t1755_i2c",
		.of_match_table = p3t1755_i2c_of_match,
	},
	.probe = p3t1755_i2c_probe,
	.id_table = p3t1755_i2c_id_table,
};
module_i2c_driver(p3t1755_driver);

MODULE_AUTHOR("Lakshay Piplani <lakshay.piplani@nxp.com>");
MODULE_DESCRIPTION("NXP P3T1750/P3T1755 I2C Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_P3T1755);
