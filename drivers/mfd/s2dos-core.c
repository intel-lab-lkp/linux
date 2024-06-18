// SPDX-License-Identifier: GPL-2.0+
/*
 * s2dos05.c - mfd core driver for the s2dos05 chip
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Copyright (C) 2024 Dzmitry Sankouski <dsankouski@gmail.com>
 *
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/mfd/core.h>
#include <linux/mfd/samsung/s2dos-core.h>
#include <linux/mfd/samsung/s2dos05.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/debugfs.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

static struct dentry *debugfs_file;

static int s2dos05_debugfs_show(struct seq_file *s, void *data)
{
	struct s2dos_core *s2dos05 = s->private;
	struct regmap *regmap = s2dos05->regmap;
	unsigned int i, reg, reg_data, pmic_id;
	int regs[] = {
		S2DOS05_REG_DEV_ID,
		S2DOS05_REG_TOPSYS_STAT,
		S2DOS05_REG_STAT,
		S2DOS05_REG_EN,
		S2DOS05_REG_LDO1_CFG,
		S2DOS05_REG_LDO2_CFG,
		S2DOS05_REG_LDO3_CFG,
		S2DOS05_REG_LDO4_CFG,
		S2DOS05_REG_BUCK_CFG,
		S2DOS05_REG_BUCK_VOUT,
		S2DOS05_REG_IRQ_MASK,
		S2DOS05_REG_SSD_TSD,
		S2DOS05_REG_OCL,
		S2DOS05_REG_IRQ
	};
	regmap_read(regmap, S2DOS05_REG_DEV_ID, &pmic_id);
	seq_printf(s, "S2DOS05, id: %d\n", pmic_id);
	seq_puts(s, "===================\n");
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		reg = regs[i];
		regmap_read(regmap, reg, &reg_data);
		seq_printf(s, "0x%02x:\t0x%02x\n", reg, reg_data);
	}

	seq_puts(s, "\n");
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(s2dos05_debugfs);

static const struct regmap_config s2dos05_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = S2DOS05_REG_IRQ,
};

static struct mfd_cell s2dos05_devs[] = {
	{ .name = "s2dos05-fg" },
	{
		.name = "s2dos05-regulator",
		.of_compatible = "samsung,s2dos05-regulator",
	},
};


static int s2dos05_i2c_probe(struct i2c_client *i2c)
{
	struct s2dos_core *s2dos05;
	struct regmap *regmap;
	struct device *dev = &i2c->dev;

	unsigned int reg_data;
	int ret = 0;

	s2dos05 = kzalloc(sizeof(struct s2dos_core), GFP_KERNEL);
	if (!s2dos05)
		return -ENOMEM;

	regmap = devm_regmap_init_i2c(i2c, &s2dos05_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(dev, "Unable to initialise I2C Regmap\n");
		return PTR_ERR(regmap);
	}
	s2dos05->regmap = regmap;

	if (regmap_read(regmap, S2DOS05_REG_DEV_ID, &reg_data) < 0) {
		dev_err(dev,
			"device not found on this channel (this is not an error)\n");
		ret = -ENODEV;
	} else {
		dev_info(dev, "%s device found with id: .0x%x\n",
				__func__, reg_data);
	}

	i2c_set_clientdata(i2c, s2dos05);

	debugfs_file = debugfs_create_file("s2dos05-regs",
				0664, NULL, (void *)s2dos05,
				  &s2dos05_debugfs_fops);
	if (!debugfs_file)
		dev_err(dev, "Failed to create debugfs file\n");

	return mfd_add_devices(dev, -1, s2dos05_devs,
			ARRAY_SIZE(s2dos05_devs), NULL, 0, NULL);
}

static const struct of_device_id s2dos05_i2c_dt_ids[] = {
	{ .compatible = "samsung,s2dos05-pmic" },
	{ },
};
MODULE_DEVICE_TABLE(of, s2dos05_i2c_dt_ids);

static struct i2c_driver s2dos05_i2c_driver = {
	.driver		= {
		.name	= "s2dos-core",
		.owner	= THIS_MODULE,
		.of_match_table	= s2dos05_i2c_dt_ids,
	},
	.probe		= s2dos05_i2c_probe,
};

module_i2c_driver(s2dos05_i2c_driver);

MODULE_DESCRIPTION("s2dos core driver");
MODULE_AUTHOR("Dzmitry Sankouski <dsankouski@gmail.com>");
MODULE_LICENSE("GPL");
