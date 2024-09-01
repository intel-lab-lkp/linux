// SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include "bmi270.h"

static int bmi270_i2c_probe(struct i2c_client *client)
{
	const char *name;
	struct regmap *regmap;
	struct device *dev = &client->dev;
	const struct i2c_device_id *id;

	regmap = devm_regmap_init_i2c(client, &bmi270_regmap_config);
	if (IS_ERR(regmap)) {
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "Failed to init i2c regmap");
	}

	id = i2c_client_get_device_id(client);
	if (id)
		name = id->name;
	else
		name = dev_name(dev);

	return bmi270_core_probe(dev, regmap, name, false);
}

static const struct i2c_device_id bmi270_i2c_id[] = {
	{"bmi270", 0},
	{}
};

static const struct of_device_id bmi270_of_match[] = {
	{.compatible = "bosch,bmi270"},
	{},
};

static struct i2c_driver bmi270_i2c_driver = {
	.driver = {
		.name = "bmi270_i2c",
		.of_match_table = bmi270_of_match,
	},
	.probe = bmi270_i2c_probe,
	.id_table = bmi270_i2c_id,
};
module_i2c_driver(bmi270_i2c_driver);

MODULE_AUTHOR("Alex Lanzano");
MODULE_DESCRIPTION("BMI270 driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_BMI270);
