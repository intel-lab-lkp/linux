// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Metanoia Communications Inc.
 *
 * Author: Jun Chang <jun.chang@metanoia-comm.com>
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

struct metanoia_usb2phy {
	struct reset_control *reset;
};

static int metanoia_usb2phy_init(struct phy *phy)
{
	struct metanoia_usb2phy *priv = phy_get_drvdata(phy);
	int ret;

	ret = reset_control_deassert(priv->reset);
	if (ret)
		dev_err(&phy->dev, "failed to reset phy: %d\n", ret);

	return ret;
}

static int metanoia_usb2phy_exit(struct phy *phy)
{
	struct metanoia_usb2phy *priv = phy_get_drvdata(phy);

	return reset_control_assert(priv->reset);
}

static const struct phy_ops metanoia_usb2phy_ops = {
	.init = metanoia_usb2phy_init,
	.exit = metanoia_usb2phy_exit,
	.owner = THIS_MODULE,
};

static int metanoia_usb2phy_probe(struct platform_device *pdev)
{
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;
	struct metanoia_usb2phy *priv;
	struct phy *phy;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->reset = devm_reset_control_get_exclusive(dev, "usb2-phy");
	if (IS_ERR(priv->reset))
		return dev_err_probe(dev, PTR_ERR(priv->reset),
				     "failed to get usb2-phy reset\n");

	phy = devm_phy_create(dev, NULL, &metanoia_usb2phy_ops);
	if (IS_ERR(phy))
		return dev_err_probe(dev, PTR_ERR(phy),
				     "failed to create usb2 phy\n");

	phy_set_drvdata(phy, priv);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct of_device_id metanoia_usb2phy_of_match[] = {
	{ .compatible = "metanoia,mt2824-usb2-phy" },
	{}
};
MODULE_DEVICE_TABLE(of, metanoia_usb2phy_of_match);

static struct platform_driver metanoia_usb2phy_driver = {
	.probe = metanoia_usb2phy_probe,
	.driver = {
		.name = "metanoia-usb2-phy",
		.of_match_table = metanoia_usb2phy_of_match,
	},
};
module_platform_driver(metanoia_usb2phy_driver);

MODULE_DESCRIPTION("Metanoia USB2 PHY driver");
MODULE_AUTHOR("Jun Chang <jun.chang@metanoia-comm.com>");
MODULE_LICENSE("GPL");
