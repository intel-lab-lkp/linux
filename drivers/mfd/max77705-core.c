// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * max77705.c - mfd core driver for the Maxim 77705
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Copyright (C) 2024 Dzmitry Sankouski <dsankouski@gmail.com>
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/mfd/core.h>
#include <linux/mfd/max77705.h>
#include <linux/mfd/max77705-private.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/debugfs.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#define I2C_ADDR_PMIC	(0xCC >> 1)	/* Top sys, Haptic */
#define I2C_ADDR_MUIC	(0x4A >> 1)
#define I2C_ADDR_CHG    (0xD2 >> 1)
#define I2C_ADDR_FG     (0x6C >> 1)
#define I2C_ADDR_DEBUG  (0xC4 >> 1)

static struct dentry *debugfs_file;

static int max77705_debugfs_show(struct seq_file *s, void *data)
{
	struct max77705_dev *max77705 = s->private;
	struct regmap *regmap = max77705->regmap;
	unsigned int i, reg, reg_data, pmic_id, pmic_rev;
	int regs[] = {
		MAX77705_PMIC_REG_MAINCTRL1,
		MAX77705_PMIC_REG_MCONFIG,
		MAX77705_PMIC_REG_MCONFIG2,
		MAX77705_PMIC_REG_INTSRC,
		MAX77705_PMIC_REG_INTSRC_MASK,
		MAX77705_PMIC_REG_SYSTEM_INT,
		MAX77705_PMIC_REG_SYSTEM_INT_MASK,
		MAX77705_RGBLED_REG_LEDEN,
		MAX77705_RGBLED_REG_LED0BRT,
		MAX77705_RGBLED_REG_LED1BRT,
		MAX77705_RGBLED_REG_LED2BRT,
		MAX77705_RGBLED_REG_LED3BRT,
		MAX77705_RGBLED_REG_LEDBLNK
	};

	regmap_read(regmap, MAX77705_PMIC_REG_PMICID1, &pmic_id);
	regmap_read(regmap, MAX77705_PMIC_REG_PMICREV, &pmic_rev);
	seq_printf(s, "MAX77705, pmic id: %d, pmic rev: %d\n",
		   pmic_id, pmic_rev);
	seq_puts(s, "===================\n");
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		reg = regs[i];
		regmap_read(regmap, reg, &reg_data);
		seq_printf(s, "0x%02x:\t0x%02x\n", reg, reg_data);
	}

	seq_puts(s, "\n");
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(max77705_debugfs);

static const struct regmap_config max77705_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77705_PMIC_REG_END,
};

static const struct regmap_config max77705_leds_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77705_LED_REG_END,
};

static const struct regmap_config max77705_fg_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77705_FG_END,
};

static struct mfd_cell max77705_devs[] = {
	{
		.name = "leds-max77705-rgb",
		.of_compatible = "maxim,max77705-led",
	},
	{
		.name = "max77705-fuelgauge",
		.of_compatible = "maxim,max77705-fg",
	},
	{
		.name = "max77705-charger",
		.of_compatible = "maxim,max77705-charger",
	},
	{
		.name = "max77705-haptic",
		.of_compatible = "maxim,max77705-haptic",
	},
};

static int max77705_i2c_probe(struct i2c_client *i2c)
{
	struct max77705_dev *max77705;
	struct max77705_platform_data *pdata = i2c->dev.platform_data;

	unsigned int reg_data;
	int ret = 0;

	max77705 = kzalloc(sizeof(struct max77705_dev), GFP_KERNEL);
	if (!max77705)
		return -ENOMEM;

	max77705->pdata = pdata;
	max77705->dev = &i2c->dev;
	max77705->i2c = i2c;
	max77705->irq = i2c->irq;

	max77705->regmap = devm_regmap_init_i2c(max77705->i2c, &max77705_regmap_config);
	if (IS_ERR(max77705->regmap)) {
		ret = PTR_ERR(max77705->regmap);
		dev_err(max77705->dev, "failed to allocate register map: %d\n",
				ret);
		return ret;
	}

	max77705->regmap_leds = devm_regmap_init_i2c(max77705->i2c, &max77705_leds_regmap_config);
	if (IS_ERR(max77705->regmap_leds)) {
		ret = PTR_ERR(max77705->regmap_leds);
		dev_err(max77705->dev, "failed to allocate register map: %d\n",
				ret);
		return ret;
	}

	i2c_set_clientdata(i2c, max77705);

	if (regmap_read(max77705->regmap, MAX77705_PMIC_REG_PMICREV, &reg_data) < 0) {
		dev_err(max77705->dev,
			"device not found on this channel (this is not an error)\n");
		ret = -ENODEV;
		goto err;
	} else {
		/* print rev */
		max77705->pmic_rev = (reg_data & MAX77705_REVISION_MASK);
		max77705->pmic_ver = ((reg_data & MAX77705_VERSION_MASK) >> MAX77705_VERSION_SHIFT);
		dev_info(max77705->dev, "%s device found: rev.0x%x, ver.0x%x\n",
				__func__, max77705->pmic_rev, max77705->pmic_ver);
	}

	max77705->charger = devm_i2c_new_dummy_device(max77705->dev, i2c->adapter, I2C_ADDR_CHG);
	i2c_set_clientdata(max77705->charger, max77705);
	max77705->regmap_charger = devm_regmap_init_i2c(max77705->charger, &max77705_regmap_config);
	if (IS_ERR(max77705->regmap)) {
		ret = PTR_ERR(max77705->regmap);
		dev_err(max77705->dev, "failed to allocate register map: %d\n",
				ret);
		return ret;
	}

	max77705->fuelgauge = devm_i2c_new_dummy_device(max77705->dev, i2c->adapter, I2C_ADDR_FG);
	i2c_set_clientdata(max77705->fuelgauge, max77705);
	max77705->regmap_fg = devm_regmap_init_i2c(max77705->fuelgauge, &max77705_fg_regmap_config);
	if (IS_ERR(max77705->regmap_fg)) {
		ret = PTR_ERR(max77705->regmap_fg);
		dev_err(max77705->dev, "failed to allocate register map: %d\n",
				ret);
		return ret;
	}

	if (likely(i2c->irq > 0))
		max77705->irq = i2c->irq;
	else {
		dev_err(max77705->dev, "failed to get irq number\n");
		return -EINVAL;
	}

	max77705->irq_base = irq_alloc_descs(-1, 0, MAX77705_IRQ_NR, -1);
	if (unlikely(max77705->irq_base < 0)) {
		dev_err(max77705->dev, "irq_alloc_descs fail: %d\n", max77705->irq_base);
		ret = -EINVAL;
		goto err;
	}

	disable_irq(max77705->irq);
	ret = max77705_irq_init(max77705);
	if (ret) {
		dev_err(max77705->dev, "failed to init irq system: %d\n", ret);
		return ret;
	}

	ret = mfd_add_devices(max77705->dev, -1, max77705_devs,
			ARRAY_SIZE(max77705_devs), NULL, 0, NULL);
	if (ret < 0)
		goto err_mfd;

	debugfs_file = debugfs_create_file("max77705-regs",
				0664, NULL, (void *)max77705,
				  &max77705_debugfs_fops);
	if (!debugfs_file)
		dev_err(max77705->dev, "Failed to create debugfs file\n");

	device_init_wakeup(max77705->dev, true);

	return ret;

err_mfd:
	mfd_remove_devices(max77705->dev);
err:
	kfree(max77705);
	return ret;
}

static void max77705_i2c_remove(struct i2c_client *i2c)
{
	struct max77705_dev *max77705 = i2c_get_clientdata(i2c);

	if (debugfs_file)
		debugfs_remove(debugfs_file);

	device_init_wakeup(max77705->dev, 0);
	mfd_remove_devices(max77705->dev);
}

static int __maybe_unused max77705_suspend(struct device *dev)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct max77705_dev *max77705 = i2c_get_clientdata(i2c);

	disable_irq(max77705->irq);
	if (device_may_wakeup(dev))
		enable_irq_wake(max77705->irq);

	return 0;
}

static int __maybe_unused max77705_resume(struct device *dev)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct max77705_dev *max77705 = i2c_get_clientdata(i2c);

	if (device_may_wakeup(dev))
		disable_irq_wake(max77705->irq);
	enable_irq(max77705->irq);

	return 0;
}

static SIMPLE_DEV_PM_OPS(max77705_pm, max77705_suspend, max77705_resume);

static const struct of_device_id max77705_i2c_dt_ids[] = {
	{ .compatible = "maxim,max77705" },
	{ },
};
MODULE_DEVICE_TABLE(of, max77705_i2c_dt_ids);

static struct i2c_driver max77705_i2c_driver = {
	.driver		= {
		.name	= MFD_DEV_NAME,
		.pm = &max77705_pm,
		.of_match_table	= max77705_i2c_dt_ids,
		.suppress_bind_attrs = true,
	},
	.probe		= max77705_i2c_probe,
	.remove		= max77705_i2c_remove,
};
module_i2c_driver(max77705_i2c_driver);

MODULE_DESCRIPTION("MAXIM 77705 multi-function core driver");
MODULE_AUTHOR("Dzmitry Sankouski <dsankouski@gmail.com>");
MODULE_LICENSE("GPL");
