// SPDX-License-Identifier: GPL-2.0
/*
 * Battery Fuel Gauge Driver for Samsung S2MU005 PMIC.
 *
 * Copyright (C) 2015 Samsung Electronics
 * Copyright (C) 2023 Yassine Oudjana <y.oudjana@protonmail.com>
 * Copyright (C) 2025 Kaustabh Chakraborty <kauschluss@disroot.org>
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#define S2MU005_FG_REG_STATUS		0x00
#define S2MU005_FG_REG_IRQ		0x02
#define S2MU005_FG_REG_RVBAT		0x04
#define S2MU005_FG_REG_RCUR_CC		0x06
#define S2MU005_FG_REG_RSOC		0x08
#define S2MU005_FG_REG_MONOUT		0x0a
#define S2MU005_FG_REG_MONOUT_SEL	0x0c
#define S2MU005_FG_REG_RBATCAP		0x0e
#define S2MU005_FG_REG_RZADJ		0x12
#define S2MU005_FG_REG_RBATZ0		0x16
#define S2MU005_FG_REG_RBATZ1		0x18
#define S2MU005_FG_REG_IRQ_LVL		0x1a
#define S2MU005_FG_REG_START		0x1e

struct s2mu005_fg {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
};

static const struct regmap_config s2mu005_fg_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static irqreturn_t s2mu005_handle_irq(int irq, void *data)
{
	struct s2mu005_fg *priv = data;

	msleep(100);
	power_supply_changed(priv->psy);

	return IRQ_HANDLED;
}

static int s2mu005_fg_get_voltage_now(struct s2mu005_fg *priv, int *value)
{
	struct regmap *regmap = priv->regmap;
	u16 reg;
	int ret;

	ret = regmap_raw_read(regmap, S2MU005_FG_REG_RVBAT, &reg, sizeof(reg));
	if (ret < 0) {
		dev_err(priv->dev, "failed to read voltage register (%d)\n", ret);
		return ret;
	}

	*value = ((unsigned long)reg * 1000000) >> 13; /* uV */

	return 0;
}

static int s2mu005_fg_get_current_now(struct s2mu005_fg *priv, int *value)
{
	struct regmap *regmap = priv->regmap;
	s16 reg;
	int ret;

	ret = regmap_raw_read(regmap, S2MU005_FG_REG_RCUR_CC, &reg, sizeof(reg));
	if (ret < 0) {
		dev_err(priv->dev, "failed to read current register (%d)\n", ret);
		return ret;
	}

	*value = -((long)reg * 1000000) >> 12; /* uA */

	return 0;
}

static int s2mu005_fg_get_capacity(struct s2mu005_fg *priv, int *value)
{
	struct regmap *regmap = priv->regmap;
	s16 reg;
	int ret;

	ret = regmap_raw_read(regmap, S2MU005_FG_REG_RSOC, &reg, sizeof(reg));
	if (ret < 0) {
		dev_err(priv->dev, "failed to read capacity register (%d)\n", ret);
		return ret;
	}

	*value = (reg * 100) >> 14; /* percentage */

	return 0;
}

static int s2mu005_fg_get_status(struct s2mu005_fg *priv, int *value)
{
	int current_now;
	int capacity;
	int ret;

	ret = s2mu005_fg_get_current_now(priv, &current_now);
	if (ret)
		return ret;

	if (current_now <= 0) {
		*value = POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	}

	ret = s2mu005_fg_get_capacity(priv, &capacity);
	if (ret)
		return ret;

	if (capacity < 90)
		*value = POWER_SUPPLY_STATUS_CHARGING;
	else
		*value = POWER_SUPPLY_STATUS_FULL;

	return 0;
}

static const enum power_supply_property s2mu005_fg_properties[] = {
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_STATUS,
};

static int s2mu005_fg_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct s2mu005_fg *priv = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = s2mu005_fg_get_voltage_now(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = s2mu005_fg_get_current_now(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = s2mu005_fg_get_capacity(priv, &val->intval);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		ret = s2mu005_fg_get_status(priv, &val->intval);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static const struct power_supply_desc s2mu005_fg_desc = {
	.name = "s2mu005-fuel-gauge",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = s2mu005_fg_properties,
	.num_properties = ARRAY_SIZE(s2mu005_fg_properties),
	.get_property = s2mu005_fg_get_property,
};

static int s2mu005_fg_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s2mu005_fg *priv;
	struct power_supply_config psy_cfg = {};
	const struct power_supply_desc *psy_desc;
	int flags;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate driver private\n");

	dev_set_drvdata(dev, priv);
	priv->dev = dev;

	priv->regmap = devm_regmap_init_i2c(client, &s2mu005_fg_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to initialize regmap\n");

	psy_desc = device_get_match_data(dev);

	psy_cfg.drv_data = priv;
	priv->psy = devm_power_supply_register(priv->dev, psy_desc, &psy_cfg);
	if (IS_ERR(priv->psy))
		return dev_err_probe(dev, PTR_ERR(priv->psy),
				     "failed to register power supply subsystem\n");

	flags = irq_get_trigger_type(client->irq);

	ret = devm_request_threaded_irq(priv->dev, client->irq, NULL,
					s2mu005_handle_irq, IRQF_ONESHOT | flags,
					psy_desc->name, priv);
	if (ret)
		dev_err_probe(dev, ret, "failed to request IRQ\n");

	return 0;
}

static const struct of_device_id s2mu005_fg_of_match_table[] = {
	{
		.compatible = "samsung,s2mu005-fuel-gauge",
		.data = &s2mu005_fg_desc,
	}, { },
};
MODULE_DEVICE_TABLE(of, s2mu005_fg_of_match_table);

static struct i2c_driver s2mu005_fg_i2c_driver = {
	.probe = s2mu005_fg_i2c_probe,
	.driver = {
		.name = "s2mu005-fuel-gauge",
		.of_match_table = of_match_ptr(s2mu005_fg_of_match_table),
	},
};
module_i2c_driver(s2mu005_fg_i2c_driver);

MODULE_DESCRIPTION("Samsung S2MU005 PMIC Battery Fuel Gauge Driver");
MODULE_AUTHOR("Yassine Oudjana <y.oudjana@protonmail.com>");
MODULE_AUTHOR("Kaustabh Chakraborty <kauschluss@disroot.org>");
MODULE_LICENSE("GPL");
