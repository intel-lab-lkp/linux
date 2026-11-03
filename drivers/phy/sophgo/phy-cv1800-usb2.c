// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Inochi Amaoto <inochiama@outlook.com>
 */

#include <linux/clk.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/usb/otg.h>
#include <linux/usb/role.h>

#define REG_USB_PHY_CTRL		0x048

#define PHY_VBUS_POWER_EN		BIT(0)
#define PHY_VBUS_POWER			BIT(1)
#define PHY_ID_OVERWRITE_EN		BIT(6)
#define PHY_ID_OVERWRITE_MODE		BIT(7)
#define PHY_ID_OVERWRITE_MODE_HOST	FIELD_PREP(BIT(7), 0)
#define PHY_ID_OVERWRITE_MODE_DEVICE	FIELD_PREP(BIT(7), 1)

#define PHY_APP_CLK_RATE		125000000
#define PHY_LPM_CLK_RATE		12000000
#define PHY_STB_CLK_RATE		333334

struct cv1800_usb_phy {
	struct phy	*phy;
	struct regmap	*syscon;
	spinlock_t	lock;
	struct clk	*usb_app_clk;
	struct clk	*usb_lpm_clk;
	struct clk	*usb_stb_clk;
	bool		support_otg;
	struct usb_role_switch	*role_sw;
	enum usb_role		default_role;
};

static int cv1800_usb_phy_set_id(struct cv1800_usb_phy *phy, enum usb_role role)
{
	unsigned int regval;

	switch (role) {
	case USB_ROLE_DEVICE:
		regval = PHY_ID_OVERWRITE_EN | PHY_ID_OVERWRITE_MODE_DEVICE;
		break;
	case USB_ROLE_HOST:
		regval = PHY_ID_OVERWRITE_EN | PHY_ID_OVERWRITE_MODE_HOST |
			 PHY_VBUS_POWER;
		break;
	default:
		return -EINVAL;
	}

	return regmap_update_bits(phy->syscon, REG_USB_PHY_CTRL,
				  PHY_ID_OVERWRITE_EN | PHY_ID_OVERWRITE_MODE |
				  PHY_VBUS_POWER, regval);
}

static int cv1800_usb_phy_role_set(struct usb_role_switch *sw, enum usb_role role)
{
	struct cv1800_usb_phy *phy = usb_role_switch_get_drvdata(sw);

	if (role == USB_ROLE_NONE)
		role = phy->default_role;

	return cv1800_usb_phy_set_id(phy, role);
}

static enum usb_role cv1800_usb_phy_role_get(struct usb_role_switch *sw)
{
	struct cv1800_usb_phy *phy = usb_role_switch_get_drvdata(sw);
	unsigned int val = 0;
	int ret = regmap_read(phy->syscon, REG_USB_PHY_CTRL, &val);

	if (ret)
		return USB_ROLE_NONE;

	if (val & PHY_ID_OVERWRITE_MODE)
		return USB_ROLE_DEVICE;

	return USB_ROLE_HOST;
}

static void cv1800_usb_phy_role_unregister(void *data)
{
	usb_role_switch_unregister(data);
}

static int cv1800_usb_phy_setup_role_switch(struct device *dev,
					    struct cv1800_usb_phy *phy)
{
	struct usb_role_switch_desc desc = {
		.fwnode = dev_fwnode(dev),
		.set = cv1800_usb_phy_role_set,
		.get = cv1800_usb_phy_role_get,
		.allow_userspace_control = true,
		.driver_data = phy,
	};
	int ret;

	if (!device_property_read_bool(dev, "usb-role-switch"))
		return 0;

	if (usb_get_role_switch_default_mode(dev) == USB_DR_MODE_HOST)
		phy->default_role = USB_ROLE_HOST;
	else
		phy->default_role = USB_ROLE_DEVICE;

	ret = cv1800_usb_phy_set_id(phy, phy->default_role);
	if (ret)
		return ret;

	phy->role_sw = usb_role_switch_register(dev, &desc);
	if (IS_ERR(phy->role_sw))
		return PTR_ERR(phy->role_sw);

	return devm_add_action_or_reset(dev, cv1800_usb_phy_role_unregister,
					phy->role_sw);
}

static int cv1800_usb_phy_set_mode(struct phy *_phy,
				   enum phy_mode mode, int submode)
{
	struct cv1800_usb_phy *phy = phy_get_drvdata(_phy);
	unsigned int regval = 0;
	int ret;

	/* The role switch owns the ID override */
	if (phy->role_sw)
		return 0;

	dev_info(&phy->phy->dev, "set mode %d", (int)mode);

	switch (mode) {
	case PHY_MODE_USB_DEVICE:
		return cv1800_usb_phy_set_id(phy, USB_ROLE_DEVICE);
	case PHY_MODE_USB_HOST:
		return cv1800_usb_phy_set_id(phy, USB_ROLE_HOST);
	case PHY_MODE_USB_OTG:
		if (!phy->support_otg)
			return 0;

		ret = regmap_read(phy->syscon, REG_USB_PHY_CTRL, &regval);
		if (ret)
			return ret;

		regval = FIELD_GET(PHY_ID_OVERWRITE_MODE, regval);
		break;
	default:
		return -EINVAL;
	}

	return regmap_update_bits(phy->syscon, REG_USB_PHY_CTRL,
				  PHY_ID_OVERWRITE_EN | PHY_ID_OVERWRITE_MODE,
				  regval);
}

static int cv1800_usb_phy_set_clock(struct cv1800_usb_phy *phy)
{
	int ret;

	ret = clk_set_rate(phy->usb_app_clk, PHY_APP_CLK_RATE);
	if (ret)
		return ret;

	ret = clk_set_rate(phy->usb_lpm_clk, PHY_LPM_CLK_RATE);
	if (ret)
		return ret;

	return clk_set_rate(phy->usb_stb_clk, PHY_STB_CLK_RATE);
}

static const struct phy_ops cv1800_usb_phy_ops = {
	.set_mode	= cv1800_usb_phy_set_mode,
	.owner		= THIS_MODULE,
};

static int cv1800_usb_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *parent = dev->parent;
	struct cv1800_usb_phy *phy;
	struct phy_provider *phy_provider;
	int ret;

	if (!parent)
		return -ENODEV;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	phy->syscon = syscon_node_to_regmap(parent->of_node);
	if (IS_ERR_OR_NULL(phy->syscon))
		return -ENODEV;

	phy->support_otg = false;

	spin_lock_init(&phy->lock);

	phy->usb_app_clk = devm_clk_get_enabled(dev, "app");
	if (IS_ERR(phy->usb_app_clk))
		return dev_err_probe(dev, PTR_ERR(phy->usb_app_clk),
			"Failed to get app clock\n");

	phy->usb_lpm_clk = devm_clk_get_enabled(dev, "lpm");
	if (IS_ERR(phy->usb_lpm_clk))
		return dev_err_probe(dev, PTR_ERR(phy->usb_lpm_clk),
			"Failed to get lpm clock\n");

	phy->usb_stb_clk = devm_clk_get_enabled(dev, "stb");
	if (IS_ERR(phy->usb_stb_clk))
		return dev_err_probe(dev, PTR_ERR(phy->usb_stb_clk),
			"Failed to get stb clock\n");

	phy->phy = devm_phy_create(dev, NULL, &cv1800_usb_phy_ops);
	if (IS_ERR(phy->phy))
		return dev_err_probe(dev, PTR_ERR(phy->phy),
			"Failed to create phy\n");

	ret = cv1800_usb_phy_set_clock(phy);
	if (ret)
		return ret;

	phy_set_drvdata(phy->phy, phy);

	ret = cv1800_usb_phy_setup_role_switch(dev, phy);
	if (ret)
		return ret;

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct of_device_id cv1800_usb_phy_ids[] = {
	{ .compatible = "sophgo,cv1800b-usb2-phy" },
	{ },
};
MODULE_DEVICE_TABLE(of, cv1800_usb_phy_ids);

static struct platform_driver cv1800_usb_phy_driver = {
	.probe = cv1800_usb_phy_probe,
	.driver = {
		.name = "cv1800-usb2-phy",
		.of_match_table = cv1800_usb_phy_ids,
	 },
};
module_platform_driver(cv1800_usb_phy_driver);

MODULE_AUTHOR("Inochi Amaoto <inochiama@outlook.com>");
MODULE_DESCRIPTION("CV1800/SG2000 SoC USB 2.0 PHY driver");
MODULE_LICENSE("GPL");
