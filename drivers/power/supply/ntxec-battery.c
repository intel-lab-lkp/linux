// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The Netronix embedded controller is a microcontroller found in some
 * e-book readers designed by the original design manufacturer Netronix, Inc.
 * It contains RTC, battery monitoring, system power management, and PWM
 * functionality.
 *
 * This driver implements battery monitoring.
 *
 * Copyright 2021 Josua Mayer <josua.mayer@jm0.eu>
 */

#include <linux/mfd/ntxec.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>

static const enum power_supply_property ntxec_battery_properties[] = {
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

struct ntxec_battery {
	struct ntxec *ec;
};

#define NTXEC_REG_READ_BATTERY	0x41

static int ntxec_battery_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct ntxec_battery *priv = power_supply_get_drvdata(psy);
	int ret;
	unsigned int value;

	switch (psp) {
		case POWER_SUPPLY_PROP_VOLTAGE_NOW:
			ret = regmap_read(priv->ec->regmap, NTXEC_REG_READ_BATTERY, &value);
			if (ret < 0)
				return ret;

			/* ec value to microvolt conversion:
			 * vendor kernel source suggests linear behaviour from 3V to 4.2V
			 * with readings 767 to 1023; each increment represents 4687,5uV.
			 * adjust 3V boundary slightly to report exactly 4.2V when full.
			 */
			val->intval = 2999872 + (value - 767) * 4688;
			break;
		default:
			dev_err(&psy->dev, "%s: invalid property %u\n", __func__, psp);
			return -EINVAL;
	}

	return 0;
}

static const struct power_supply_desc ntxec_battery_desc = {
	.name = "ec-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = ntxec_battery_properties,
	.get_property = ntxec_battery_get_property,
	.num_properties = ARRAY_SIZE(ntxec_battery_properties),
};

static int ntxec_battery_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ntxec *ec = dev_get_drvdata(dev->parent);
	struct power_supply_config psy_cfg = {};
	struct ntxec_battery *priv;
	struct power_supply *psy;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->ec = ec;
	psy_cfg.drv_data = priv;
	psy_cfg.fwnode = dev_fwnode(dev->parent);
	psy_cfg.no_wakeup_source = true;
	psy = devm_power_supply_register(dev, &ntxec_battery_desc, &psy_cfg);
	if (IS_ERR(psy))
		return PTR_ERR(psy);

	return 0;
}

static struct platform_driver ntxec_battery_driver = {
	.driver = {
		.name = "ntxec-battery",
	},
	.probe = ntxec_battery_probe,
};
module_platform_driver(ntxec_battery_driver);

MODULE_AUTHOR("Josua Mayer <josua.mayer@jm0.eu>");
MODULE_DESCRIPTION("Battery driver for Netronix EC");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ntxec-battery");
