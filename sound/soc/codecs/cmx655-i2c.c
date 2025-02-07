// SPDX-License-Identifier: GPL-2.0-only

#include <linux/version.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>

#include "cmx655.h"

static int cmx655_i2c_probe(struct i2c_client *client)
{
	int ret;
	struct regmap *regmap = devm_regmap_init_i2c(client, &cmx655_regmap);

	ret =
	    cmx655_common_register_component(&client->dev,
					     regmap,
					     client->irq);
	if (ret < 0) {
		dev_err(&client->dev,
			"%s: Register component failed %d\n", __func__, ret);
	}

	return ret;
};

static void cmx655_i2c_remove(struct i2c_client *client)
{
	cmx655_common_unregister_component(&client->dev);
};

static const struct i2c_device_id cmx655_device_id[] = {
	{ "cmx655", 0 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, cmx655_device_id);

static const struct of_device_id cmx655_of_match[] = {
	{.compatible = "cml,cmx655d" },
	{ }
};

MODULE_DEVICE_TABLE(of, cmx655_of_match);

static struct i2c_driver cmx655_i2c_driver = {
	.probe = cmx655_i2c_probe,
	.remove = cmx655_i2c_remove,
	.driver = {
		   .name = "cmx655",
		   .of_match_table = cmx655_of_match,
		    },
	.id_table = cmx655_device_id
};

module_i2c_driver(cmx655_i2c_driver);

MODULE_DESCRIPTION("ASoC CMX655 driver, I2C adapter");
MODULE_AUTHOR("CML");
MODULE_LICENSE("GPL");
