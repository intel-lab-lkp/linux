// SPDX-License-Identifier: GPL-2.0
/*
 * pf1550.c - mfd core driver for the PF1550
 *
 * Copyright (C) 2016 Freescale Semiconductor, Inc.
 * Robin Gong <yibin.gong@freescale.com>
 *
 * This driver is based on max77693.c
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mfd/core.h>
#include <linux/mfd/pf1550.h>
#include <linux/of.h>
#include <linux/regmap.h>

static const struct mfd_cell pf1550_devs[] = {
	{
		.name = "pf1550-regulator",
		.of_compatible = "fsl,pf1550-regulator",
	},
	{
		.name = "pf1550-onkey",
		.of_compatible = "fsl,pf1550-onkey",
	},
	{
		.name = "pf1550-charger",
		.of_compatible = "fsl,pf1550-charger",
	},
};

static const struct regmap_config pf1550_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = PF1550_PMIC_REG_END,
};

static const struct regmap_irq pf1550_regulator_irqs[] = {
	REGMAP_IRQ_REG(PF1550_PMIC_IRQ_SW1_LS,	       0, PMIC_IRQ_SW1_LS),
	REGMAP_IRQ_REG(PF1550_PMIC_IRQ_SW2_LS,	       0, PMIC_IRQ_SW2_LS),
	REGMAP_IRQ_REG(PF1550_PMIC_IRQ_SW3_LS,	       0, PMIC_IRQ_SW3_LS),
	REGMAP_IRQ_REG(PF1550_PMIC_IRQ_SW1_HS,	       3, PMIC_IRQ_SW1_HS),
	REGMAP_IRQ_REG(PF1550_PMIC_IRQ_SW2_HS,	       3, PMIC_IRQ_SW2_HS),
	REGMAP_IRQ_REG(PF1550_PMIC_IRQ_SW3_HS,	       3, PMIC_IRQ_SW3_HS),
	REGMAP_IRQ_REG(PF1550_PMIC_IRQ_LDO1_FAULT,    16, PMIC_IRQ_LDO1_FAULT),
	REGMAP_IRQ_REG(PF1550_PMIC_IRQ_LDO2_FAULT,    16, PMIC_IRQ_LDO2_FAULT),
	REGMAP_IRQ_REG(PF1550_PMIC_IRQ_LDO3_FAULT,    16, PMIC_IRQ_LDO3_FAULT),
	REGMAP_IRQ_REG(PF1550_PMIC_IRQ_TEMP_110,      22, PMIC_IRQ_TEMP_110),
	REGMAP_IRQ_REG(PF1550_PMIC_IRQ_TEMP_125,      22, PMIC_IRQ_TEMP_125),
};

static const struct regmap_irq_chip pf1550_regulator_irq_chip = {
	.name			= "pf1550-regulator",
	.status_base		= PF1550_PMIC_REG_SW_INT_STAT0,
	.mask_base		= PF1550_PMIC_REG_SW_INT_MASK0,
	.num_regs		= 23,
	.irqs			= pf1550_regulator_irqs,
	.num_irqs		= ARRAY_SIZE(pf1550_regulator_irqs)
};

static const struct regmap_irq pf1550_onkey_irqs[] = {
	REGMAP_IRQ_REG(PF1550_ONKEY_IRQ_PUSHI,  0, ONKEY_IRQ_PUSHI),
	REGMAP_IRQ_REG(PF1550_ONKEY_IRQ_1SI,	0, ONKEY_IRQ_1SI),
	REGMAP_IRQ_REG(PF1550_ONKEY_IRQ_2SI,	0, ONKEY_IRQ_2SI),
	REGMAP_IRQ_REG(PF1550_ONKEY_IRQ_3SI,	0, ONKEY_IRQ_3SI),
	REGMAP_IRQ_REG(PF1550_ONKEY_IRQ_4SI,	0, ONKEY_IRQ_4SI),
	REGMAP_IRQ_REG(PF1550_ONKEY_IRQ_8SI,    0, ONKEY_IRQ_8SI),
};

static const struct regmap_irq_chip pf1550_onkey_irq_chip = {
	.name			= "pf1550-onkey",
	.status_base		= PF1550_PMIC_REG_ONKEY_INT_STAT0,
	.ack_base		= PF1550_PMIC_REG_ONKEY_INT_STAT0,
	.mask_base		= PF1550_PMIC_REG_ONKEY_INT_MASK0,
	.use_ack                = 1,
	.init_ack_masked	= 1,
	.num_regs		= 1,
	.irqs			= pf1550_onkey_irqs,
	.num_irqs		= ARRAY_SIZE(pf1550_onkey_irqs),
};

static const struct regmap_irq pf1550_charger_irqs[] = {
	REGMAP_IRQ_REG(PF1550_CHARG_IRQ_BAT2SOCI,	0, CHARG_IRQ_BAT2SOCI),
	REGMAP_IRQ_REG(PF1550_CHARG_IRQ_BATI,		0, CHARG_IRQ_BATI),
	REGMAP_IRQ_REG(PF1550_CHARG_IRQ_CHGI,		0, CHARG_IRQ_CHGI),
	REGMAP_IRQ_REG(PF1550_CHARG_IRQ_VBUSI,		0, CHARG_IRQ_VBUSI),
	REGMAP_IRQ_REG(PF1550_CHARG_IRQ_THMI,		0, CHARG_IRQ_THMI),
};

static const struct regmap_irq_chip pf1550_charger_irq_chip = {
	.name			= "pf1550-charger",
	.status_base		= PF1550_CHARG_REG_CHG_INT,
	.mask_base		= PF1550_CHARG_REG_CHG_INT_MASK,
	.num_regs		= 1,
	.irqs			= pf1550_charger_irqs,
	.num_irqs		= ARRAY_SIZE(pf1550_charger_irqs),
};

int pf1550_read_otp(struct pf1550_dev *pf1550, unsigned int index,
		    unsigned int *val)
{
	int ret = 0;

	ret = regmap_write(pf1550->regmap, PF1550_PMIC_REG_KEY, 0x15);
	if (ret)
		goto read_err;
	ret = regmap_write(pf1550->regmap, PF1550_CHARG_REG_CHGR_KEY2, 0x50);
	if (ret)
		goto read_err;
	ret = regmap_write(pf1550->regmap, PF1550_TEST_REG_KEY3, 0xAB);
	if (ret)
		goto read_err;
	ret = regmap_write(pf1550->regmap, PF1550_TEST_REG_FMRADDR, index);
	if (ret)
		goto read_err;
	ret = regmap_read(pf1550->regmap, PF1550_TEST_REG_FMRDATA, val);
	if (ret)
		goto read_err;

	return 0;

read_err:
	dev_err(pf1550->dev, "read otp reg %x found!\n", index);
	return ret;
}

static int pf1550_i2c_probe(struct i2c_client *i2c)
{
	struct pf1550_dev *pf1550;
	unsigned int reg_data = 0;
	int ret = 0;

	pf1550 = devm_kzalloc(&i2c->dev,
			      sizeof(struct pf1550_dev), GFP_KERNEL);
	if (!pf1550)
		return -ENOMEM;

	i2c_set_clientdata(i2c, pf1550);
	pf1550->dev = &i2c->dev;
	pf1550->i2c = i2c;
	pf1550->irq = i2c->irq;

	pf1550->regmap = devm_regmap_init_i2c(i2c, &pf1550_regmap_config);
	if (IS_ERR(pf1550->regmap)) {
		ret = PTR_ERR(pf1550->regmap);
		dev_err(pf1550->dev, "failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	ret = regmap_read(pf1550->regmap, PF1550_PMIC_REG_DEVICE_ID, &reg_data);
	if (ret < 0 || reg_data != PF1550_DEVICE_ID) {
		dev_err(pf1550->dev, "device not found!\n");
		return ret;
	}

	pf1550->type = PF1550;
	dev_info(pf1550->dev, "pf1550 found.\n");

	ret = devm_regmap_add_irq_chip(pf1550->dev, pf1550->regmap,
				       pf1550->irq,
				IRQF_ONESHOT | IRQF_SHARED |
				IRQF_TRIGGER_FALLING, 0,
				&pf1550_regulator_irq_chip,
				&pf1550->irq_data_regulator);
	if (ret) {
		dev_err(pf1550->dev, "failed to add irq1 chip: %d\n", ret);
		return ret;
	}

	ret = devm_regmap_add_irq_chip(pf1550->dev, pf1550->regmap,
				       pf1550->irq,
				IRQF_ONESHOT | IRQF_SHARED |
				IRQF_TRIGGER_FALLING, 0,
				&pf1550_onkey_irq_chip,
				&pf1550->irq_data_onkey);
	if (ret) {
		dev_err(pf1550->dev, "failed to add irq3 chip: %d\n", ret);
		return ret;
	}

	ret = devm_regmap_add_irq_chip(pf1550->dev, pf1550->regmap,
				       pf1550->irq,
				IRQF_ONESHOT | IRQF_SHARED |
				IRQF_TRIGGER_FALLING, 0,
				&pf1550_charger_irq_chip,
				&pf1550->irq_data_charger);
	if (ret) {
		dev_err(pf1550->dev, "failed to add irq4 chip: %d\n", ret);
		return ret;
	}

	return devm_mfd_add_devices(pf1550->dev, -1, pf1550_devs,
			      ARRAY_SIZE(pf1550_devs), NULL, 0, NULL);
}

static const struct i2c_device_id pf1550_i2c_id[] = {
	{ "pf1550", PF1550 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, pf1550_i2c_id);

static int pf1550_suspend(struct device *dev)
{
	struct i2c_client *i2c = container_of(dev, struct i2c_client, dev);
	struct pf1550_dev *pf1550 = i2c_get_clientdata(i2c);

	if (device_may_wakeup(dev)) {
		enable_irq_wake(pf1550->irq);
		disable_irq(pf1550->irq);
	}

	return 0;
}

static int pf1550_resume(struct device *dev)
{
	struct i2c_client *i2c = container_of(dev, struct i2c_client, dev);
	struct pf1550_dev *pf1550 = i2c_get_clientdata(i2c);

	if (device_may_wakeup(dev)) {
		disable_irq_wake(pf1550->irq);
		enable_irq(pf1550->irq);
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(pf1550_pm, pf1550_suspend, pf1550_resume);

static const struct of_device_id pf1550_dt_match[] = {
	{ .compatible = "fsl,pf1550" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pf1550_dt_match);

static struct i2c_driver pf1550_i2c_driver = {
	.driver = {
		   .name = "pf1550",
		   .pm = pm_sleep_ptr(&pf1550_pm),
		   .of_match_table = of_match_ptr(pf1550_dt_match),
	},
	.probe = pf1550_i2c_probe,
	.id_table = pf1550_i2c_id,
};

module_i2c_driver(pf1550_i2c_driver);

MODULE_DESCRIPTION("Freescale PF1550 multi-function core driver");
MODULE_AUTHOR("Robin Gong <yibin.gong@freescale.com>");
MODULE_LICENSE("GPL v2");
