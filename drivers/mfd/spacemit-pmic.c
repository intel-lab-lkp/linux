// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 by RISCstar Solutions Corporation.  All rights reserved.
 * Derived from code from:
 *	Copyright (C) 2024 Troy Mitchell <troymitchell988@gmail.com>
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/types.h>

struct spacemit_pmic_data {
	const struct regmap_config *regmap_config;
	const struct mfd_cell *mfd_cells;	/* array */
	size_t mfd_cell_count;
};

static const struct regmap_config p1_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= 0xaa,
};

/* The name field defines the *driver* name that should bind to the device */
static const struct mfd_cell p1_cells[] = {
	{
		.name		= "spacemit-p1-regulator",
	},
};

static const struct spacemit_pmic_data p1_pmic_data = {
	.regmap_config	= &p1_regmap_config,
	.mfd_cells	= p1_cells,
	.mfd_cell_count	= ARRAY_SIZE(p1_cells),
};

static int spacemit_pmic_probe(struct i2c_client *client)
{
	const struct spacemit_pmic_data *data;
	struct device *dev = &client->dev;
	struct regmap *regmap;

	/* We currently have no need for a device-specific structure */
	data = of_device_get_match_data(dev);
	regmap = devm_regmap_init_i2c(client, data->regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "regmap initialization failed");

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO,
				    data->mfd_cells, data->mfd_cell_count,
				    NULL, 0, NULL);
}

static const struct of_device_id spacemit_pmic_match[] = {
	{
		.compatible	= "spacemit,p1",
		.data		= &p1_pmic_data,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, spacemit_pmic_match);

static struct i2c_driver spacemit_pmic_i2c_driver = {
	.driver = {
		.name = "spacemit-pmic",
		.of_match_table = spacemit_pmic_match,
	},
	.probe    = spacemit_pmic_probe,
};

static int __init spacemit_pmic_init(void)
{
	return i2c_add_driver(&spacemit_pmic_i2c_driver);
}

static void __exit spacemit_pmic_exit(void)
{
	i2c_del_driver(&spacemit_pmic_i2c_driver);
}

module_init(spacemit_pmic_init);
module_exit(spacemit_pmic_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SpacemiT multi-function PMIC driver");
