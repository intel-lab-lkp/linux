// SPDX-License-Identifier: GPL-2.0-only
/*
 * SpacemiT K1 USB 2.0 PHY driver
 *
 * Copyright (C) 2025 SpacemiT (Hangzhou) Technology Co. Ltd
 * Copyright (C) 2025 Ze Huang <huangze9015@gmail.com>
 */

#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/usb/of.h>

#define USB2_PHY_REG01			0x04
#define  USB2_PHY_REG01_VAL		0x60ef
#define  USB2_PHY_REG01_PLL_IS_READY	BIT(0)
#define USB2_PHY_REG04			0x10
#define  USB2_PHY_REG04_AUTO_CLEAR_DIS	BIT(2)
#define USB2_PHY_REG0D			0x34
#define  USB2_PHY_REG0D_VAL		0x1c
#define USB2_PHY_REG26			0x98
#define  USB2_PHY_REG26_VAL		0xbec4

#define USB2D_CTRL_RESET_TIME_MS	50

struct spacemit_usb2phy {
	struct phy	*phy;
	struct clk	*clk;
	struct regmap	*regmap_base;
};

static const struct regmap_config phy_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x200,
};

static int spacemit_usb2phy_init(struct phy *phy)
{
	struct spacemit_usb2phy *sphy = phy_get_drvdata(phy);
	struct regmap *map = sphy->regmap_base;
	u32 val;
	int ret;

	ret = clk_prepare_enable(sphy->clk);
	if (ret) {
		dev_err(&phy->dev, "failed to enable clock\n");
		return ret;
	}

	/*
	 * make sure the usb controller is not under reset process before
	 * any configuration
	 */
	usleep_range(150, 200);
	regmap_write(map, USB2_PHY_REG26, USB2_PHY_REG26_VAL); /* 24M ref clk */

	ret = regmap_read_poll_timeout(map, USB2_PHY_REG01, val,
				       (val & USB2_PHY_REG01_PLL_IS_READY),
				       500, USB2D_CTRL_RESET_TIME_MS * 1000);
	if (ret) {
		dev_err(&phy->dev, "wait PHY_REG01[PLLREADY] timeout\n");
		return ret;
	}

	/* release usb2 phy internal reset and enable clock gating */
	regmap_write(map, USB2_PHY_REG01, USB2_PHY_REG01_VAL);
	regmap_write(map, USB2_PHY_REG0D, USB2_PHY_REG0D_VAL);

	/* auto clear host disc */
	regmap_update_bits(map, USB2_PHY_REG04, USB2_PHY_REG04_AUTO_CLEAR_DIS,
			   USB2_PHY_REG04_AUTO_CLEAR_DIS);

	return 0;
}

static int spacemit_usb2phy_exit(struct phy *phy)
{
	struct spacemit_usb2phy *sphy = phy_get_drvdata(phy);

	clk_disable_unprepare(sphy->clk);

	return 0;
}

static const struct phy_ops spacemit_usb2phy_ops = {
	.init = spacemit_usb2phy_init,
	.exit = spacemit_usb2phy_exit,
	.owner = THIS_MODULE,
};

static int spacemit_usb2phy_probe(struct platform_device *pdev)
{
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;
	struct spacemit_usb2phy *sphy;
	void __iomem *base;

	sphy = devm_kzalloc(dev, sizeof(*sphy), GFP_KERNEL);
	if (!sphy)
		return -ENOMEM;

	sphy->clk = devm_clk_get_prepared(&pdev->dev, NULL);
	if (IS_ERR(sphy->clk))
		return dev_err_probe(dev, PTR_ERR(sphy->clk), "Failed to get clock\n");

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	sphy->regmap_base = devm_regmap_init_mmio(dev, base, &phy_regmap_config);
	if (IS_ERR(sphy->regmap_base))
		return dev_err_probe(dev, PTR_ERR(sphy->regmap_base),
				     "Failed to init regmap\n");

	sphy->phy = devm_phy_create(dev, NULL, &spacemit_usb2phy_ops);
	if (IS_ERR(sphy->phy))
		return dev_err_probe(dev, PTR_ERR(sphy->phy), "Failed to create phy\n");

	phy_set_drvdata(sphy->phy, sphy);
	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct of_device_id spacemit_usb2phy_dt_match[] = {
	{ .compatible = "spacemit,k1-usb2-phy", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, spacemit_usb2phy_dt_match);

static struct platform_driver spacemit_usb2_phy_driver = {
	.probe	= spacemit_usb2phy_probe,
	.driver = {
		.name   = "spacemit-usb2-phy",
		.of_match_table = spacemit_usb2phy_dt_match,
	},
};
module_platform_driver(spacemit_usb2_phy_driver);

MODULE_DESCRIPTION("Spacemit USB 2.0 PHY driver");
MODULE_LICENSE("GPL");
