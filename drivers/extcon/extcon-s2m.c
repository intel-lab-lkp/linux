// SPDX-License-Identifier: GPL-2.0
/*
 * Extcon Driver for Samsung S2M series PMICs.
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd
 * Copyright (C) 2026 Kaustabh Chakraborty <kauschluss@disroot.org>
 */

#include <linux/delay.h>
#include <linux/extcon-provider.h>
#include <linux/interrupt.h>
#include <linux/mfd/samsung/core.h>
#include <linux/mfd/samsung/s2mu005.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

struct s2m_muic {
	struct device *dev;
	struct regmap *regmap;
	struct extcon_dev *extcon;
	struct s2m_muic_irq_data *irq_data;
	const unsigned int *extcon_cable;
	bool attached;
};

struct s2m_muic_irq_data {
	const char *name;
	int (*const handler)(struct s2m_muic *);
	int irq;
};

static int s2mu005_muic_detach(struct s2m_muic *priv)
{
	int ret;
	int i;

	ret = regmap_set_bits(priv->regmap, S2MU005_REG_MUIC_CTRL1,
			      S2MU005_MUIC_MAN_SW);
	if (ret < 0) {
		dev_err(priv->dev, "failed to disable manual switching\n");
		return ret;
	}

	ret = regmap_set_bits(priv->regmap, S2MU005_REG_MUIC_CTRL3,
			      S2MU005_MUIC_ONESHOT_ADC);
	if (ret < 0) {
		dev_err(priv->dev, "failed to enable ADC oneshot mode\n");
		return ret;
	}

	ret = regmap_clear_bits(priv->regmap, S2MU005_REG_MUIC_SWCTRL, ~0);
	if (ret < 0) {
		dev_err(priv->dev, "failed to clear switch control register\n");
		return ret;
	}

	/* Find all set states and clear them */
	for (i = 0; priv->extcon_cable[i]; i++) {
		unsigned int state = priv->extcon_cable[i];

		if (extcon_get_state(priv->extcon, state) == true)
			extcon_set_state_sync(priv->extcon, state, false);
	}

	priv->attached = false;

	return 0;
}

static int s2mu005_muic_attach(struct s2m_muic *priv)
{
	unsigned int type;
	int ret;

	/* If any device is already attached, detach it */
	if (priv->attached) {
		s2mu005_muic_detach(priv);
		msleep(100);
	}

	ret = regmap_read(priv->regmap, S2MU005_REG_MUIC_DEV1, &type);
	if (ret < 0) {
		dev_err(priv->dev, "failed to read DEV1 register\n");
		return ret;
	}

	/*
	 * All USB connections which require communication via its D+
	 * and D- wires need it.
	 */
	if (type & (S2MU005_MUIC_OTG | S2MU005_MUIC_DCP | S2MU005_MUIC_SDP)) {
		ret = regmap_update_bits(priv->regmap, S2MU005_REG_MUIC_SWCTRL,
					 S2MU005_MUIC_DM_DP,
					 FIELD_PREP(S2MU005_MUIC_DM_DP,
						    S2MU005_MUIC_DM_DP_USB));
		if (ret < 0) {
			dev_err(priv->dev, "failed to configure DM/DP pins\n");
			return ret;
		}
	}

	/*
	 * For OTG connections, enable manual switching and ADC oneshot
	 * mode. Since the port will now be supplying power, the
	 * internal ADC (measuring the ID-GND resistance) is made to
	 * poll periodically for any changes, so as to prevent any
	 * damages due to power.
	 */
	if (type & S2MU005_MUIC_OTG) {
		ret = regmap_clear_bits(priv->regmap, S2MU005_REG_MUIC_CTRL1,
					S2MU005_MUIC_MAN_SW);
		if (ret < 0) {
			dev_err(priv->dev, "failed to enable manual switching\n");
			return ret;
		}

		ret = regmap_clear_bits(priv->regmap, S2MU005_REG_MUIC_CTRL3,
					S2MU005_MUIC_ONESHOT_ADC);
		if (ret < 0) {
			dev_err(priv->dev, "failed to disable ADC oneshot mode\n");
			return ret;
		}
	}

	switch (type) {
	case S2MU005_MUIC_OTG:
		dev_dbg(priv->dev, "USB OTG connection detected\n");
		extcon_set_state_sync(priv->extcon, EXTCON_USB_HOST, true);
		priv->attached = true;
		break;
	case S2MU005_MUIC_CDP:
		dev_dbg(priv->dev, "USB CDP connection detected\n");
		extcon_set_state_sync(priv->extcon, EXTCON_USB, true);
		extcon_set_state_sync(priv->extcon, EXTCON_CHG_USB_CDP, true);
		priv->attached = true;
		break;
	case S2MU005_MUIC_SDP:
		dev_dbg(priv->dev, "USB SDP connection detected\n");
		extcon_set_state_sync(priv->extcon, EXTCON_USB, true);
		extcon_set_state_sync(priv->extcon, EXTCON_CHG_USB_SDP, true);
		priv->attached = true;
		break;
	case S2MU005_MUIC_DCP:
		dev_dbg(priv->dev, "USB DCP connection detected\n");
		extcon_set_state_sync(priv->extcon, EXTCON_USB, true);
		extcon_set_state_sync(priv->extcon, EXTCON_CHG_USB_DCP, true);
		priv->attached = true;
		break;
	case S2MU005_MUIC_UART:
		dev_dbg(priv->dev, "UART connection detected\n");
		extcon_set_state_sync(priv->extcon, EXTCON_JIG, true);
		priv->attached = true;
		break;
	}

	if (!priv->attached)
		dev_warn(priv->dev, "failed to recognize the device attached\n");

	return ret;
}

static int s2mu005_muic_init(struct s2m_muic *priv)
{
	int ret = 0;

	ret = regmap_update_bits(priv->regmap, S2MU005_REG_MUIC_LDOADC_L,
				 S2MU005_MUIC_VSET,
				 FIELD_PREP(S2MU005_MUIC_VSET,
					    S2MU005_MUIC_VSET_3P0V));
	if (ret < 0) {
		dev_err(priv->dev, "failed to set internal ADC voltage regulator\n");
		return ret;
	}

	ret = regmap_update_bits(priv->regmap, S2MU005_REG_MUIC_LDOADC_H,
				 S2MU005_MUIC_VSET,
				 FIELD_PREP(S2MU005_MUIC_VSET,
					    S2MU005_MUIC_VSET_3P0V));
	if (ret < 0) {
		dev_err(priv->dev, "failed to set internal ADC voltage regulator\n");
		return ret;
	}

	ret = regmap_clear_bits(priv->regmap, S2MU005_REG_MUIC_CTRL1,
				S2MU005_MUIC_IRQ);
	if (ret < 0) {
		dev_err(priv->dev, "failed to enable MUIC interrupts\n");
		return ret;
	}

	return s2mu005_muic_attach(priv);
}

static const unsigned int s2mu005_muic_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_CHG_USB_SDP,
	EXTCON_CHG_USB_DCP,
	EXTCON_CHG_USB_CDP,
	EXTCON_JIG,
	EXTCON_NONE,
};

static struct s2m_muic_irq_data s2mu005_muic_irq_data[] = {
	{
		.name = "attach",
		.handler = s2mu005_muic_attach
	}, {
		.name = "detach",
		.handler = s2mu005_muic_detach
	}, {
		/* sentinel */
	}
};

static irqreturn_t s2m_muic_irq_func(int virq, void *data)
{
	struct s2m_muic *priv = data;
	const struct s2m_muic_irq_data *irq_data = priv->irq_data;
	int ret;
	int i;

	for (i = 0; irq_data[i].handler; i++) {
		if (virq != irq_data[i].irq)
			continue;

		ret = irq_data[i].handler(priv);
		if (ret < 0)
			dev_err(priv->dev, "failed to handle interrupt for %s (%d)\n",
				irq_data[i].name, ret);
		break;
	}

	return IRQ_HANDLED;
}

static int s2m_muic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sec_pmic_dev *pmic_drvdata = dev_get_drvdata(dev->parent);
	struct s2m_muic *priv;
	int ret;
	int i;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	priv->dev = dev;
	priv->regmap = pmic_drvdata->regmap_pmic;

	switch (platform_get_device_id(pdev)->driver_data) {
	case S2MU005:
		priv->extcon_cable = s2mu005_muic_extcon_cable;
		priv->irq_data = s2mu005_muic_irq_data;
		/* Initialize MUIC */
		ret = s2mu005_muic_init(priv);
		break;
	default:
		return dev_err_probe(dev, -ENODEV,
				     "device type %d is not supported by driver\n",
				     pmic_drvdata->device_type);
	}
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to initialize MUIC\n");

	priv->extcon = devm_extcon_dev_allocate(dev, priv->extcon_cable);
	if (IS_ERR(priv->extcon))
		return dev_err_probe(dev, PTR_ERR(priv->extcon),
				     "failed to allocate memory for extcon\n");

	ret = devm_extcon_dev_register(dev, priv->extcon);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register extcon device\n");

	for (i = 0; priv->irq_data[i].handler; i++) {
		int irq = platform_get_irq_byname_optional(pdev,
							   priv->irq_data[i].name);
		if (irq == -ENXIO)
			continue;
		if (irq <= 0)
			return dev_err_probe(dev, -EINVAL, "failed to get IRQ %s\n",
					     priv->irq_data[i].name);

		priv->irq_data[i].irq = irq;
		ret = devm_request_threaded_irq(dev, irq, NULL,
						s2m_muic_irq_func, IRQF_ONESHOT,
						priv->irq_data[i].name, priv);
		if (ret)
			return dev_err_probe(dev, ret, "failed to request IRQ\n");
	}

	return 0;
}

static void s2m_muic_remove(struct platform_device *pdev)
{
	struct s2m_muic *priv = dev_get_drvdata(&pdev->dev);

	/*
	 * Disabling the MUIC device is important as it disables manual
	 * switching mode, thereby enabling auto switching mode.
	 *
	 * This is to ensure that when the board is powered off, it
	 * goes into LPM charging mode when a USB charger is connected.
	 */
	switch (platform_get_device_id(pdev)->driver_data) {
	case S2MU005:
		s2mu005_muic_detach(priv);
		break;
	}
}

static const struct platform_device_id s2m_muic_id_table[] = {
	{ "s2mu005-muic", S2MU005 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(platform, s2m_muic_id_table);

static const struct of_device_id s2m_muic_of_match_table[] = {
	{
		.compatible = "samsung,s2mu005-muic",
		.data = (void *)S2MU005,
	}, {
		/* sentinel */
	},
};
MODULE_DEVICE_TABLE(of, s2m_muic_of_match_table);

static struct platform_driver s2m_muic_driver = {
	.driver = {
		.name = "s2m-muic",
	},
	.probe = s2m_muic_probe,
	.remove = s2m_muic_remove,
	.id_table = s2m_muic_id_table,
};
module_platform_driver(s2m_muic_driver);

MODULE_DESCRIPTION("Extcon Driver For Samsung S2M Series PMICs");
MODULE_AUTHOR("Kaustabh Chakraborty <kauschluss@disroot.org>");
MODULE_LICENSE("GPL");
