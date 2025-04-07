// SPDX-License-Identifier: GPL-2.0-only

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include "zl3073x.h"

static const struct i2c_device_id zl3073x_i2c_id[] = {
	{ "zl3073x-i2c", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(i2c, zl3073x_i2c_id);

static const struct of_device_id zl3073x_i2c_of_match[] = {
	{ .compatible = "microchip,zl3073x-i2c" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, zl3073x_i2c_of_match);

static int zl3073x_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	const struct i2c_device_id *id;
	struct zl3073x_dev *zldev;
	int rc = 0;

	zldev = zl3073x_dev_alloc(dev);
	if (!zldev)
		return -ENOMEM;

	id = i2c_client_get_device_id(client);
	zldev->dev = dev;

	zldev->regmap = devm_regmap_init_i2c(client,
					     zl3073x_get_regmap_config());
	if (IS_ERR(zldev->regmap)) {
		rc = PTR_ERR(zldev->regmap);
		dev_err(dev, "Failed to allocate register map: %d\n", rc);
		return rc;
	}

	i2c_set_clientdata(client, zldev);

	return zl3073x_dev_init(zldev);
}

static void zl3073x_i2c_remove(struct i2c_client *client)
{
	struct zl3073x_dev *zldev;

	zldev = i2c_get_clientdata(client);
	zl3073x_dev_exit(zldev);
}

static struct i2c_driver zl3073x_i2c_driver = {
	.driver = {
		.name = "zl3073x-i2c",
		.of_match_table = of_match_ptr(zl3073x_i2c_of_match),
	},
	.probe = zl3073x_i2c_probe,
	.remove = zl3073x_i2c_remove,
	.id_table = zl3073x_i2c_id,
};

module_i2c_driver(zl3073x_i2c_driver);

MODULE_AUTHOR("Ivan Vecera <ivecera@redhat.com>");
MODULE_DESCRIPTION("Microchip ZL3073x I2C driver");
MODULE_IMPORT_NS("ZL3073X");
MODULE_LICENSE("GPL");
