// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/usb.h>
#include <linux/usb/chipidea.h>
#include <linux/usb/phy.h>

struct tegra_usb_device {
	struct ci_hdrc_platform_data data;
	struct platform_device *dev;
};

struct tegra_xmm6260_phy {
	struct device *dev;
	struct platform_device *usb_dev;
	struct usb_phy *usb_phy;
	struct gpio_desc *enable_gpio;
};

static int tegra_xmm6260_phy_power_on(struct phy *phy)
{
	struct tegra_xmm6260_phy *mphy = phy_get_drvdata(phy);
	struct tegra_usb_device *usb = platform_get_drvdata(mphy->usb_dev);
	int ret;

	gpiod_set_value_cansleep(mphy->enable_gpio, 1);

	ret = usb_phy_init(mphy->usb_phy);
	if (ret) {
		gpiod_set_value_cansleep(mphy->enable_gpio, 0);
		return dev_err_probe(mphy->dev, ret,
				     "failed to init USB PHY\n");
	}

	usb->dev = ci_hdrc_add_device(&mphy->usb_dev->dev,
				      mphy->usb_dev->resource,
				      mphy->usb_dev->num_resources,
				      &usb->data);
	if (IS_ERR(usb->dev)) {
		gpiod_set_value_cansleep(mphy->enable_gpio, 0);
		usb_phy_shutdown(mphy->usb_phy);
		return dev_err_probe(mphy->dev, PTR_ERR(usb->dev),
				     "failed to register USB controller\n");
	}

	return 0;
}

static int tegra_xmm6260_phy_power_off(struct phy *phy)
{
	struct tegra_xmm6260_phy *mphy = phy_get_drvdata(phy);
	struct tegra_usb_device *usb = platform_get_drvdata(mphy->usb_dev);

	ci_hdrc_remove_device(usb->dev);
	usb_phy_shutdown(mphy->usb_phy);

	gpiod_set_value_cansleep(mphy->enable_gpio, 0);

	return 0;
}

static const struct phy_ops tegra_xmm6260_phy_ops = {
	.power_on = tegra_xmm6260_phy_power_on,
	.power_off = tegra_xmm6260_phy_power_off,
	.owner = THIS_MODULE,
};

static int tegra_xmm6260_phy_probe(struct platform_device *pdev)
{
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;
	struct device_node *usb_node;
	struct phy *generic_phy;
	struct tegra_xmm6260_phy *mphy;

	mphy = devm_kzalloc(dev, sizeof(*mphy), GFP_KERNEL);
	if (!mphy)
		return -ENOMEM;

	mphy->enable_gpio = devm_gpiod_get_optional(dev, "enable",
						    GPIOD_OUT_LOW);
	if (IS_ERR(mphy->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(mphy->enable_gpio),
				     "failed to get enable GPIO\n");

	usb_node = of_parse_phandle(dev->of_node, "nvidia,usb-bus", 0);
	if (IS_ERR(usb_node))
		return dev_err_probe(dev, PTR_ERR(usb_node),
				     "failed to parse modem USB bus\n");

	mphy->usb_dev = of_find_device_by_node(usb_node);
	of_node_put(usb_node);
	if (!mphy->usb_dev)
		return dev_err_probe(dev, -ENODEV,
				     "failed to get modem USB bus\n");

	mphy->usb_phy = devm_usb_get_phy_by_phandle(dev, "nvidia,usb-bus", 1);
	if (IS_ERR(mphy->usb_phy))
		return dev_err_probe(dev, PTR_ERR(mphy->usb_phy),
				     "failed to get USB PHY");

	generic_phy = devm_phy_create(dev, NULL, &tegra_xmm6260_phy_ops);
	if (IS_ERR(generic_phy))
		return dev_err_probe(dev, PTR_ERR(generic_phy),
				     "failed to create PHY\n");

	phy_set_drvdata(generic_phy, mphy);
	mphy->dev = dev;

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(phy_provider))
		return dev_err_probe(dev, PTR_ERR(phy_provider),
				     "failed to register PHY\n");

	return 0;
}

static const struct of_device_id tegra_xmm6260_phy_match[] = {
	{ .compatible = "nvidia,tegra-xmm6260" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tegra_xmm6260_phy_match);

static struct platform_driver tegra_xmm6260_phy_driver = {
	.driver = {
		.name = "tegra-xmm6260-phy",
		.of_match_table = tegra_xmm6260_phy_match,
	},
	.probe = tegra_xmm6260_phy_probe,
};
module_platform_driver(tegra_xmm6260_phy_driver);

MODULE_AUTHOR("Svyatolsav Ryhel <clamor95@gmail.com>");
MODULE_DESCRIPTION("Tegra XMM6260 PHY driver");
MODULE_LICENSE("GPL");
