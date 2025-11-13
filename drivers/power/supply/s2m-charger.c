// SPDX-License-Identifier: GPL-2.0
/*
 * Battery Charger Driver for Samsung S2M series PMICs.
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd
 * Copyright (c) 2025 Kaustabh Chakraborty <kauschluss@disroot.org>
 */

#include <linux/devm-helpers.h>
#include <linux/delay.h>
#include <linux/extcon.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mfd/samsung/core.h>
#include <linux/mfd/samsung/irq.h>
#include <linux/mfd/samsung/s2mu005.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

struct s2m_chgr {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct extcon_dev *extcon;
	struct work_struct extcon_work;
	struct notifier_block extcon_nb;
};

static int s2mu005_chgr_get_online(struct s2m_chgr *priv, int *value)
{
	u32 val;
	int ret = 0;

	ret = regmap_read(priv->regmap, S2MU005_REG_CHGR_STATUS0, &val);
	if (ret < 0) {
		dev_err(priv->dev, "failed to read register (%d)\n", ret);
		return ret;
	}

	*value = !!(val & S2MU005_CHGR_CHG);

	return ret;
}

static int s2mu005_chgr_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct s2m_chgr *priv = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		ret = s2mu005_chgr_get_online(priv, &val->intval);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static void s2mu005_chgr_extcon_work(struct work_struct *work)
{
	struct s2m_chgr *priv = container_of(work, struct s2m_chgr,
						 extcon_work);
	int ret;

	if (extcon_get_state(priv->extcon, EXTCON_USB_HOST) == true) {
		ret = regmap_update_bits(priv->regmap, S2MU005_REG_CHGR_CTRL0,
					 S2MU005_CHGR_OP_MODE,
					 S2MU005_CHGR_OP_MODE_OTG);
		if (ret < 0)
			dev_err(priv->dev, "failed to set operation mode to OTG (%d)\n",
				ret);

		goto psy_update;
	}

	if (extcon_get_state(priv->extcon, EXTCON_USB) == true) {
		ret = regmap_update_bits(priv->regmap, S2MU005_REG_CHGR_CTRL0,
					 S2MU005_CHGR_OP_MODE,
					 S2MU005_CHGR_OP_MODE_CHG);
		if (ret < 0)
			dev_err(priv->dev, "failed to set operation mode to charging (%d)\n",
				ret);

		goto psy_update;
	}

	ret = regmap_clear_bits(priv->regmap, S2MU005_REG_CHGR_CTRL0,
				S2MU005_CHGR_OP_MODE);
	if (ret < 0)
		dev_err(priv->dev, "failed to clear operation mode (%d)\n", ret);

psy_update:
	power_supply_changed(priv->psy);
}

static const enum power_supply_property s2mu005_chgr_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc s2mu005_chgr_psy_desc = {
	.name = "s2mu005-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = s2mu005_chgr_properties,
	.num_properties = ARRAY_SIZE(s2mu005_chgr_properties),
	.get_property = s2mu005_chgr_get_property,
};

static int s2m_chgr_extcon_notifier(struct notifier_block *nb,
					unsigned long event, void *param)
{
	struct s2m_chgr *priv = container_of(nb, struct s2m_chgr, extcon_nb);

	schedule_work(&priv->extcon_work);

	return NOTIFY_OK;
}

static int s2m_chgr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sec_pmic_dev *pmic_drvdata = dev_get_drvdata(dev->parent);
	struct s2m_chgr *priv;
	struct device_node *extcon_node;
	struct power_supply_config psy_cfg = {};
	const struct power_supply_desc *psy_desc;
	work_func_t extcon_work_func;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return dev_err_probe(dev, -ENOMEM, "failed to allocate driver private\n");

	platform_set_drvdata(pdev, priv);
	priv->dev = dev;
	priv->regmap = pmic_drvdata->regmap_pmic;

	switch (platform_get_device_id(pdev)->driver_data) {
	case S2MU005:
		psy_desc = &s2mu005_chgr_psy_desc;
		extcon_work_func = s2mu005_chgr_extcon_work;
		break;
	default:
		return dev_err_probe(dev, -ENODEV,
				     "device type %d is not supported by driver\n",
				     pmic_drvdata->device_type);
	}

	psy_cfg.drv_data = priv;
	priv->psy = devm_power_supply_register(dev, psy_desc, &psy_cfg);
	if (IS_ERR(priv->psy))
		return dev_err_probe(dev, PTR_ERR(priv->psy),
				     "failed to register power supply subsystem\n");

	/* MUIC is mandatory. If unavailable, request probe deferral */
	extcon_node = of_get_child_by_name(dev->parent->of_node, "extcon");
	priv->extcon = extcon_find_edev_by_node(extcon_node);
	if (IS_ERR(priv->extcon))
		return -EPROBE_DEFER;

	ret = devm_work_autocancel(dev, &priv->extcon_work, extcon_work_func);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize extcon work\n");

	priv->extcon_nb.notifier_call = s2m_chgr_extcon_notifier;
	ret = devm_extcon_register_notifier_all(dev, priv->extcon, &priv->extcon_nb);
	if (ret)
		dev_err_probe(dev, ret, "failed to register extcon notifier\n");

	return 0;
}

static const struct platform_device_id s2m_chgr_id_table[] = {
	{ "s2mu005-charger", S2MU005 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(platform, s2m_chgr_id_table);

#ifdef CONFIG_OF
/*
 * Device is instantiated through parent MFD device and device matching
 * is done through platform_device_id.
 *
 * However if device's DT node contains proper compatible and driver is
 * built as a module, then the *module* matching will be done through DT
 * aliases. This requires of_device_id table. In the same time this will
 * not change the actual *device* matching so do not add .of_match_table.
 */
static const struct of_device_id s2m_chgr_of_match_table[] = {
	{
		.compatible = "samsung,s2mu005-charger",
		.data = (void *)S2MU005,
	}, {
		/* sentinel */
	},
};
MODULE_DEVICE_TABLE(of, s2m_chgr_of_match_table);
#endif

static struct platform_driver s2m_chgr_driver = {
	.driver = {
		.name = "s2m-charger",
	},
	.probe = s2m_chgr_probe,
	.id_table = s2m_chgr_id_table,
};
module_platform_driver(s2m_chgr_driver);

MODULE_DESCRIPTION("Battery Charger Driver For Samsung S2M Series PMICs");
MODULE_AUTHOR("Kaustabh Chakraborty <kauschluss@disroot.org>");
MODULE_LICENSE("GPL");
