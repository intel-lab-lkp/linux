// SPDX-License-Identifier: GPL-2.0-only

#include <linux/dev_printk.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/mfd/zl3073x.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include "zl3073x.h"

static int zl3073x_i2c_probe(struct i2c_client *client)
{
	struct regmap_config regmap_cfg;
	struct device *dev = &client->dev;
	struct zl3073x_dev *zldev;

	zldev = zl3073x_devm_alloc(dev);
	if (IS_ERR(zldev))
		return PTR_ERR(zldev);

	zl3073x_dev_init_regmap_config(&regmap_cfg);

	zldev->regmap = devm_regmap_init_i2c(client, &regmap_cfg);
	if (IS_ERR(zldev->regmap)) {
		dev_err_probe(dev, PTR_ERR(zldev->regmap),
			      "Failed to initialize regmap\n");
		return PTR_ERR(zldev->regmap);
	}

	/* Initialize device and use I2C address as dev ID */
	return zl3073x_dev_probe(zldev, i2c_get_match_data(client),
				 client->addr);
}

static const struct i2c_device_id zl3073x_i2c_id[] = {
	{ "zl30731", .driver_data = (kernel_ulong_t)&zl3073x_chip_info[ZL30731] },
	{ "zl30732", .driver_data = (kernel_ulong_t)&zl3073x_chip_info[ZL30732] },
	{ "zl30733", .driver_data = (kernel_ulong_t)&zl3073x_chip_info[ZL30733] },
	{ "zl30734", .driver_data = (kernel_ulong_t)&zl3073x_chip_info[ZL30734] },
	{ "zl30735", .driver_data = (kernel_ulong_t)&zl3073x_chip_info[ZL30735] },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, zl3073x_i2c_id);

static const struct of_device_id zl3073x_i2c_of_match[] = {
	{ .compatible = "microchip,zl30731", .data = &zl3073x_chip_info[ZL30731] },
	{ .compatible = "microchip,zl30732", .data = &zl3073x_chip_info[ZL30732] },
	{ .compatible = "microchip,zl30733", .data = &zl3073x_chip_info[ZL30733] },
	{ .compatible = "microchip,zl30734", .data = &zl3073x_chip_info[ZL30734] },
	{ .compatible = "microchip,zl30735", .data = &zl3073x_chip_info[ZL30735] },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, zl3073x_i2c_of_match);

static struct i2c_driver zl3073x_i2c_driver = {
	.driver = {
		.name = "zl3073x-i2c",
		.of_match_table = zl3073x_i2c_of_match,
	},
	.probe = zl3073x_i2c_probe,
	.id_table = zl3073x_i2c_id,
};
module_i2c_driver(zl3073x_i2c_driver);

MODULE_AUTHOR("Ivan Vecera <ivecera@redhat.com>");
MODULE_DESCRIPTION("Microchip ZL3073x I2C driver");
MODULE_IMPORT_NS("ZL3073X");
MODULE_LICENSE("GPL");
