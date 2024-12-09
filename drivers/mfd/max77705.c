// SPDX-License-Identifier: GPL-2.0+
//
// Maxim MAX77705 PMIC core driver
//
// Copyright (C) 2024 Dzmitry Sankouski <dsankouski@gmail.com>

#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/core.h>
#include <linux/mfd/max77705-private.h>
#include <linux/mfd/max77693-common.h>
#include <linux/pm.h>
#include <linux/power/max17042_battery.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/of.h>

#define I2C_ADDR_FG     0x36
#define	FUEL_GAUGE_NAME "max77705-battery"

const struct dev_pm_ops max77705_pm_ops;

static struct mfd_cell max77705_devs[] = {
	{
		.name = "max77705-rgb",
		.of_compatible = "maxim,max77705-rgb",
	},
	{
		.name = "max77705-charger",
		.of_compatible = "maxim,max77705-charger",
	},
	{
		.name = "max77705-haptic",
		.of_compatible = "maxim,max77705-haptic",
	},
	{
		.name = FUEL_GAUGE_NAME,
	}
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
	struct i2c_client *i2c_fg;
	struct regmap_irq_chip_data *irq_data;
	struct irq_domain *domain;
	int ret;
	unsigned int pmic_rev_value;
	enum max77705_hw_rev pmic_rev;

	max77705 = devm_kzalloc(&i2c->dev, sizeof(*max77705), GFP_KERNEL);
	if (!max77705)
		return -ENOMEM;

	max77705->i2c = i2c;
	max77705->dev = &i2c->dev;
	max77705->irq = i2c->irq;
	max77705->type = TYPE_MAX77705;
	i2c_set_clientdata(i2c, max77705);

	max77705->regmap = devm_regmap_init_i2c(i2c, &max77705_regmap_config);
	if (IS_ERR(max77705->regmap))
		return PTR_ERR(max77705->regmap);

	if (regmap_read(max77705->regmap, MAX77705_PMIC_REG_PMICREV, &pmic_rev_value) < 0)
		return -ENODEV;

	pmic_rev = pmic_rev_value & MAX77705_REVISION_MASK;
	if (pmic_rev != MAX77705_PASS3)
		return dev_err_probe(max77705->dev, -ENODEV,
				"Rev.0x%x is not tested\n", pmic_rev);

	max77705->regmap_leds = devm_regmap_init_i2c(i2c, &max77705_leds_regmap_config);
	if (IS_ERR(max77705->regmap_leds))
		return dev_err_probe(max77705->dev, PTR_ERR(max77705->regmap_leds),
				"Failed to register leds regmap\n");

	ret = devm_regmap_add_irq_chip(max77705->dev, max77705->regmap,
					max77705->irq,
					IRQF_ONESHOT | IRQF_SHARED, 0,
					&max77705_topsys_irq_chip,
					&irq_data);
	if (ret)
		return dev_err_probe(max77705->dev, ret, "Failed to add irq chip\n");

	/* Unmask interrupts from all blocks in interrupt source register */
	ret = regmap_update_bits(max77705->regmap,
				 MAX77705_PMIC_REG_INTSRC_MASK,
				 MAX77705_SRC_IRQ_ALL, (unsigned int)~MAX77705_SRC_IRQ_ALL);
	if (ret < 0)
		return dev_err_probe(max77705->dev, ret,
			"Could not unmask interrupts in INTSRC\n");

	domain = regmap_irq_get_domain(irq_data);

	i2c_fg = devm_i2c_new_dummy_device(max77705->dev, max77705->i2c->adapter, I2C_ADDR_FG);
	if (IS_ERR(i2c_fg))
		return dev_err_probe(max77705->dev, PTR_ERR(i2c_fg), "Failed to add irq chip\n");

	for (int i = 0;; i++) {
		if (i >= ARRAY_SIZE(max77705_devs))
			return -EINVAL;

		if (!strcmp(max77705_devs[i].name, FUEL_GAUGE_NAME)) {
			max77705_devs[i].platform_data = &i2c_fg;
			max77705_devs[i].pdata_size = sizeof(i2c_fg);
			break;
		}
	}

	ret = devm_mfd_add_devices(max77705->dev, PLATFORM_DEVID_NONE,
				   max77705_devs, ARRAY_SIZE(max77705_devs),
				   NULL, 0, domain);
	if (ret)
		return dev_err_probe(max77705->dev, ret, "Failed to register child devices\n");

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

static const struct of_device_id max77705_i2c_of_match[] = {
	{ .compatible = "maxim,max77705" },
	{ },
};
MODULE_DEVICE_TABLE(of, max77705_i2c_of_match);

static struct i2c_driver max77705_i2c_driver = {
	.driver = {
		.name			= "max77705",
		.of_match_table		= max77705_i2c_of_match,
		.pm			= pm_sleep_ptr(&max77705_pm_ops),
		.suppress_bind_attrs	= true,
	},
	.probe = max77705_i2c_probe,
};
module_i2c_driver(max77705_i2c_driver);

MODULE_DESCRIPTION("Maxim MAX77705 PMIC core driver");
MODULE_AUTHOR("Dzmitry Sankouski <dsankouski@gmail.com>");
MODULE_LICENSE("GPL");
