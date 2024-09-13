// SPDX-License-Identifier: GPL-2.0+
//
// max77705.c - mfd core driver for the MAX77705
//
// Copyright (C) 2024 Dzmitry Sankouski <dsankouski@gmail.com>

#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/mfd/max77705-private.h>
#include <linux/mfd/max77693-common.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#define I2C_ADDR_CHG    (0xD2 >> 1)
#define I2C_ADDR_FG     (0x6C >> 1)

static struct mfd_cell max77705_devs[] = {
	{
		.name = "leds-max77705-rgb",
		.of_compatible = "maxim,max77705-led",
	},
	{
		.name = "max77705-fuel-gauge",
		.of_compatible = "maxim,max77705-fuel-gauge",
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

static const struct regmap_range max77705_readable_ranges[] = {
	regmap_reg_range(MAX77705_PMIC_REG_PMICID1,		MAX77705_PMIC_REG_BSTOUT_MASK),
	regmap_reg_range(MAX77705_PMIC_REG_INTSRC,		MAX77705_PMIC_REG_RESERVED_29),
	regmap_reg_range(MAX77705_PMIC_REG_BOOSTCONTROL1,	MAX77705_PMIC_REG_BOOSTCONTROL1),
	regmap_reg_range(MAX77705_PMIC_REG_MCONFIG,		MAX77705_PMIC_REG_MCONFIG2),
	regmap_reg_range(MAX77705_PMIC_REG_FORCE_EN_MASK,	MAX77705_PMIC_REG_FORCE_EN_MASK),
	regmap_reg_range(MAX77705_PMIC_REG_BOOSTCONTROL1,	MAX77705_PMIC_REG_BOOSTCONTROL1),
	regmap_reg_range(MAX77705_PMIC_REG_BOOSTCONTROL2,	MAX77705_PMIC_REG_BOOSTCONTROL2),
	regmap_reg_range(MAX77705_PMIC_REG_SW_RESET,		MAX77705_PMIC_REG_USBC_RESET),
};

static const struct regmap_range max77705_writable_ranges[] = {
	regmap_reg_range(MAX77705_PMIC_REG_MAINCTRL1,		MAX77705_PMIC_REG_BSTOUT_MASK),
	regmap_reg_range(MAX77705_PMIC_REG_INTSRC,		MAX77705_PMIC_REG_RESERVED_29),
	regmap_reg_range(MAX77705_PMIC_REG_BOOSTCONTROL1,	MAX77705_PMIC_REG_BOOSTCONTROL1),
	regmap_reg_range(MAX77705_PMIC_REG_MCONFIG,		MAX77705_PMIC_REG_MCONFIG2),
	regmap_reg_range(MAX77705_PMIC_REG_FORCE_EN_MASK,	MAX77705_PMIC_REG_FORCE_EN_MASK),
	regmap_reg_range(MAX77705_PMIC_REG_BOOSTCONTROL1,	MAX77705_PMIC_REG_BOOSTCONTROL1),
	regmap_reg_range(MAX77705_PMIC_REG_BOOSTCONTROL2,	MAX77705_PMIC_REG_BOOSTCONTROL2),
	regmap_reg_range(MAX77705_PMIC_REG_SW_RESET,		MAX77705_PMIC_REG_USBC_RESET),

};

static const struct regmap_access_table max77705_readable_table = {
	.yes_ranges = max77705_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(max77705_readable_ranges),
};

static const struct regmap_access_table max77705_writable_table = {
	.yes_ranges = max77705_writable_ranges,
	.n_yes_ranges = ARRAY_SIZE(max77705_writable_ranges),
};

static const struct regmap_config max77705_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.rd_table = &max77705_readable_table,
	.wr_table = &max77705_writable_table,
	.max_register = MAX77705_PMIC_REG_USBC_RESET,
};

static const struct regmap_config max77705_leds_regmap_config = {
	.reg_base = MAX77705_RGBLED_REG_BASE,
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77705_LED_REG_END,
};

static const struct regmap_config max77705_chg_regmap_config = {
	.reg_base = MAX77705_CHG_REG_BASE,
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77705_CHG_REG_SAFEOUT_CTRL,
};

static const struct regmap_config max77705_fg_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77705_FG_END,
};

static const struct regmap_irq max77705_topsys_irqs[] = {
	{ .mask = MAX77705_SYSTEM_IRQ_BSTEN_INT,  },
	{ .mask = MAX77705_SYSTEM_IRQ_SYSUVLO_INT,  },
	{ .mask = MAX77705_SYSTEM_IRQ_SYSOVLO_INT,  },
	{ .mask = MAX77705_SYSTEM_IRQ_TSHDN_INT,  },
	{ .mask = MAX77705_SYSTEM_IRQ_TM_INT,  },
};

static const struct regmap_irq_chip max77705_topsys_irq_chip = {
	.name			= "max77705-topsys",
	.status_base		= MAX77705_PMIC_REG_SYSTEM_INT,
	.mask_base		= MAX77705_PMIC_REG_SYSTEM_INT_MASK,
	.num_regs		= 1,
	.irqs			= max77705_topsys_irqs,
	.num_irqs		= ARRAY_SIZE(max77705_topsys_irqs),
};

static int max77705_i2c_probe(struct i2c_client *i2c)
{
	struct max77693_dev *max77705;
	struct i2c_client *i2c_chg;
	struct i2c_client *i2c_fg;
	struct regmap_irq_chip_data *irq_data;
	struct irq_domain *domain;
	int ret;
	unsigned int pmic_rev_value;
	u8 pmic_ver, pmic_rev;


	max77705 = devm_kzalloc(&i2c->dev, sizeof(struct max77693_dev),
				GFP_KERNEL);
	if (!max77705)
		return -ENOMEM;

	max77705->dev = &i2c->dev;
	max77705->irq = i2c->irq;
	max77705->type = TYPE_MAX77705;
	i2c_set_clientdata(i2c, max77705);

	max77705->regmap = devm_regmap_init_i2c(i2c, &max77705_regmap_config);
	if (IS_ERR(max77705->regmap))
		return PTR_ERR(max77705->regmap);

	if (regmap_read(max77705->regmap, MAX77705_PMIC_REG_PMICREV, &pmic_rev_value) < 0)
		return -ENODEV;

	pmic_rev = (pmic_rev_value & MAX77705_REVISION_MASK);
	pmic_ver = ((pmic_rev_value & MAX77705_VERSION_MASK) >> MAX77705_VERSION_SHIFT);
	dev_dbg(max77705->dev, "device found: rev.0x%x, ver.0x%x\n",
		pmic_rev, pmic_ver);
	if (pmic_rev != MAX77705_PASS3) {
		dev_err(max77705->dev, "rev.0x%x is not tested",
			pmic_rev);
		return -ENODEV;
	}

	max77705->regmap_leds = devm_regmap_init_i2c(i2c, &max77705_leds_regmap_config);
	if (IS_ERR(max77705->regmap_leds))
		return PTR_ERR(max77705->regmap_leds);

	i2c_chg = devm_i2c_new_dummy_device(max77705->dev,
						i2c->adapter, I2C_ADDR_CHG);
	max77705->regmap_chg = devm_regmap_init_i2c(i2c_chg,
						    &max77705_chg_regmap_config);
	if (IS_ERR(max77705->regmap_chg))
		return PTR_ERR(max77705->regmap_chg);

	i2c_fg = devm_i2c_new_dummy_device(max77705->dev, i2c->adapter,
						I2C_ADDR_FG);
	max77705->regmap_fg = devm_regmap_init_i2c(i2c_fg,
						   &max77705_fg_regmap_config);
	if (IS_ERR(max77705->regmap_fg))
		return PTR_ERR(max77705->regmap_fg);

	ret = devm_regmap_add_irq_chip(max77705->dev, max77705->regmap,
					max77705->irq,
					IRQF_ONESHOT | IRQF_SHARED, 0,
					&max77705_topsys_irq_chip,
					&irq_data);
	if (ret)
		dev_err(max77705->dev, "failed to add irq chip: %d\n", ret);

	/* Unmask interrupts from all blocks in interrupt source register */
	ret = regmap_update_bits(max77705->regmap,
				 MAX77705_PMIC_REG_INTSRC_MASK,
				 MAX77705_SRC_IRQ_ALL, (unsigned int)~MAX77705_SRC_IRQ_ALL);
	if (ret < 0)
		dev_err(max77705->dev,
			"Could not unmask interrupts in INTSRC: %d\n", ret);

	domain = regmap_irq_get_domain(irq_data);
	ret = devm_mfd_add_devices(max77705->dev, PLATFORM_DEVID_NONE,
				   max77705_devs, ARRAY_SIZE(max77705_devs),
				   NULL, 0, domain);
	if (ret) {
		dev_err(max77705->dev, "failed to add MFD devices: %d\n", ret);
		return ret;
	}

	device_init_wakeup(max77705->dev, true);

	return 0;
}

static int max77705_suspend(struct device *dev)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct max77693_dev *max77705 = i2c_get_clientdata(i2c);

	disable_irq(max77705->irq);
	if (device_may_wakeup(dev))
		enable_irq_wake(max77705->irq);

	return 0;
}

static int max77705_resume(struct device *dev)
{
	struct i2c_client *i2c = to_i2c_client(dev);
	struct max77693_dev *max77705 = i2c_get_clientdata(i2c);

	if (device_may_wakeup(dev))
		disable_irq_wake(max77705->irq);
	enable_irq(max77705->irq);

	return 0;
}

DEFINE_SIMPLE_DEV_PM_OPS(max77705_pm_ops, max77705_suspend, max77705_resume);

static const struct of_device_id max77705_i2c_dt_ids[] = {
	{ .compatible = "maxim,max77705" },
	{ },
};
MODULE_DEVICE_TABLE(of, max77705_i2c_dt_ids);

static struct i2c_driver max77705_i2c_driver = {
	.driver		= {
		.name		= "max77705",
		.of_match_table	= max77705_i2c_dt_ids,
		.pm		= pm_sleep_ptr(&max77705_pm_ops),
		.suppress_bind_attrs = true,
	},
	.probe		= max77705_i2c_probe,
};
module_i2c_driver(max77705_i2c_driver);

MODULE_DESCRIPTION("MAXIM 77705 multi-function core driver");
MODULE_AUTHOR("Dzmitry Sankouski <dsankouski@gmail.com>");
MODULE_LICENSE("GPL");
