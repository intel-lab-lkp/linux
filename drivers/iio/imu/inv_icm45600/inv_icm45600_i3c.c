// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 InvenSense, Inc.
 */

#include <linux/i3c/device.h>
#include <linux/i3c/master.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "inv_icm45600.h"

static const struct regmap_config inv_icm45600_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static const struct i3c_device_id inv_icm45600_i3c_ids[] = {
	I3C_DEVICE_EXTRA_INFO(0x0235, 0x0000, 0x0011, (void *)NULL),
	I3C_DEVICE_EXTRA_INFO(0x0235, 0x0000, 0x0084, (void *)NULL),
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(i3c, inv_icm45600_i3c_ids);

static const uint8_t inv_icm45600_i3c_code[INV_CHIP_NB] = {
	[INV_CHIP_ICM45605] = INV_ICM45600_WHOAMI_ICM45605,
	[INV_CHIP_ICM45686] = INV_ICM45600_WHOAMI_ICM45686,
	[INV_CHIP_ICM45688P] = INV_ICM45600_WHOAMI_ICM45688P,
	[INV_CHIP_ICM45608] = INV_ICM45600_WHOAMI_ICM45608,
	[INV_CHIP_ICM45634] = INV_ICM45600_WHOAMI_ICM45634,
	[INV_CHIP_ICM45689] = INV_ICM45600_WHOAMI_ICM45689,
	[INV_CHIP_ICM45606] = INV_ICM45600_WHOAMI_ICM45606,
	[INV_CHIP_ICM45687] = INV_ICM45600_WHOAMI_ICM45687
};

static int inv_icm45600_i3c_probe(struct i3c_device *i3cdev)
{
	int ret;
	unsigned int whoami;
	enum inv_icm45600_chip chip;
	struct regmap *regmap;

	regmap = devm_regmap_init_i3c(i3cdev, &inv_icm45600_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&i3cdev->dev, "Failed to register i3c regmap %ld\n", PTR_ERR(regmap));
		return PTR_ERR(regmap);
	}

	ret = regmap_read(regmap, INV_ICM45600_REG_WHOAMI, &whoami);
	if (ret) {
		dev_err(&i3cdev->dev, "Failed to read part id %d\n", whoami);
		return ret;
	}

	for (chip = INV_CHIP_ICM45605; chip < INV_CHIP_NB; chip++) {
		if (whoami == inv_icm45600_i3c_code[chip])
			break;
	}

	if (chip == INV_CHIP_NB) {
		dev_err(&i3cdev->dev, "Failed to match part id %d\n", whoami);
		return -ENODEV;
	}

	return inv_icm45600_core_probe(regmap, chip, false, NULL);
}

static struct i3c_driver inv_icm45600_driver = {
	.driver = {
		.name = "inv_icm45600_i3c",
		.pm = pm_sleep_ptr(&inv_icm45600_pm_ops),
	},
	.probe = inv_icm45600_i3c_probe,
	.id_table = inv_icm45600_i3c_ids,
};
module_i3c_driver(inv_icm45600_driver);

MODULE_AUTHOR("Remi Buisson <remi.buisson@tdk.com>");
MODULE_DESCRIPTION("InvenSense ICM-456xx i3c driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_ICM45600");
