// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for Innosilicon USB3.0/PCIe phy found in Allwinner A523 processors.
 * Currently, the driver only supports the USB3.0 part.
 *
 * Copyright (C) 2025 Mikhail Kalashnikov <iuncuim@gmail.com>
 * Based on phy-sun50i-usb3.c, which is:
 * Copyright (C) 2017 Icenowy Zheng <icenowy@aosc.io>
 *
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#define PHY_SYS_VER			0x00

#define PHY_USB3_BGR		0x08
/* Control bits for the USB3 part */
#define USB3_RESETN			BIT(0)
#define USB3_ACLK_EN		BIT(17)
#define USB3_HCLK_EN		BIT(16)

#define PHY_CTL				0x10
/* Control bits for the common part */
#define PHY_RSTN			BIT(0)
/* Bit for selecting internal(0) or external(1) clock */
#define PHY_CLK_SEL			BIT(30)
/* Bit for selecting PCIe(0) or USB3(1) role */
#define PHY_USE_SEL			BIT(31)

#define PHY_CLK_OFFSET		0x80000

struct sun55i_usb3_pcie_phy {
	struct device *dev;
	struct phy *phy;
	void __iomem *regs;
	void __iomem *regs_clk;
	struct reset_control *reset;
	struct clk *clk;
};

/*
 * These values are derived from the manufacturer's driver code.
 * Comments are preserved.
 */
static void sun55i_usb3_phy_open(struct sun55i_usb3_pcie_phy *phy)
{
	u32 val;

	val = readl(phy->regs_clk + 0x1418);
	val &= ~(GENMASK(17, 16));
	val |= BIT(25);
	writel(val, phy->regs_clk + 0x1418);

	/* reg_rx_eq_bypass[3]=1, rx_ctle_res_cal_bypass */
	val = readl(phy->regs_clk + 0x674);
	val |= BIT(3);
	writel(val, phy->regs_clk + 0x674);

	/* rx_ctle_res_cal=0xf, 0x4->0xf */
	val = readl(phy->regs_clk + 0x704);
	val |= BIT(8) | BIT(9) | BIT(11);
	writel(val, phy->regs_clk + 0x704);

	/* CDR_div_fin_gain1 */
	val = readl(phy->regs_clk + 0x400);
	val |= BIT(4);
	writel(val, phy->regs_clk + 0x400);

	/* CDR_div1_fin_gain1 */
	val = readl(phy->regs_clk + 0x404);
	val |= GENMASK(3, 0);
	val |= BIT(5);
	writel(val, phy->regs_clk + 0x404);

	/* CDR_div3_fin_gain1 */
	val = readl(phy->regs_clk + 0x408);
	val |= BIT(5);
	writel(val, phy->regs_clk + 0x408);

	val = readl(phy->regs_clk + 0x109c);
	val |= BIT(1);
	writel(val, phy->regs_clk + 0x109c);

	/* SSC configure */
	/* div_N */
	val = readl(phy->regs_clk + 0x107c);
	val &= ~(GENMASK(17, 12));
	val |= BIT(12);
	writel(val, phy->regs_clk + 0x107c);

	/* modulation freq div */
	val = readl(phy->regs_clk + 0x1020);
	val &= ~(GENMASK(4, 0));
	val |= BIT(1) | BIT(2);
	writel(val, phy->regs_clk + 0x1020);

	/* spread[6:0], 400*9=4410ppm ssc */
	val = readl(phy->regs_clk + 0x1034);
	val &= ~(GENMASK(22, 16));
	val |= BIT(16) | BIT(19);
	writel(val, phy->regs_clk + 0x1034);

	val = readl(phy->regs_clk + 0x101c);
	/* don't disable ssc = 0 */
	val &= ~BIT(28);
	/* choose downspread */
	val |= BIT(27);
	writel(val, phy->regs_clk + 0x101c);
}

static int sun55i_usb3_pcie_clk_init(struct sun55i_usb3_pcie_phy *phy)
{
	u32 val;
	int ret;

	ret = clk_prepare_enable(phy->clk);
	if (ret)
		return ret;

	ret = reset_control_deassert(phy->reset);
	if (ret) {
		clk_disable_unprepare(phy->clk);
		return ret;
	}

	val = readl(phy->regs + PHY_CTL);
	val |= PHY_USE_SEL | PHY_RSTN;
	val &= ~PHY_CLK_SEL;
	writel(val, phy->regs + PHY_CTL);

	val = readl(phy->regs + PHY_USB3_BGR);
	val |= USB3_ACLK_EN | USB3_HCLK_EN | USB3_RESETN;
	writel(val, phy->regs + PHY_USB3_BGR);

	return 0;
}

static int sun55i_usb3_pcie_phy_init(struct phy *_phy)
{
	struct sun55i_usb3_pcie_phy *phy = phy_get_drvdata(_phy);

	sun55i_usb3_phy_open(phy);

	return 0;
}

static int sun55i_usb3_pcie_phy_exit(struct phy *_phy)
{
	struct sun55i_usb3_pcie_phy *phy = phy_get_drvdata(_phy);

	reset_control_assert(phy->reset);
	clk_disable_unprepare(phy->clk);

	return 0;
}

static void sun55i_usb3_pcie_phy_power_set(struct phy *_phy, bool on)
{
	struct sun55i_usb3_pcie_phy *phy = phy_get_drvdata(_phy);
	u32 val;

	val = readl(phy->regs_clk + 0x14);
	val = on ? (val & ~BIT(26)) : (val | BIT(26));
	writel(val, phy->regs_clk + 0x14);

	val = readl(phy->regs_clk);
	val = on ? (val & ~BIT(10)) : (val | BIT(10));
	writel(val, phy->regs_clk);
}

static int sun55i_usb3_pcie_phy_power_on(struct phy *_phy)
{
	sun55i_usb3_pcie_phy_power_set(_phy, true);

	return 0;
}

static int sun55i_usb3_pcie_phy_power_off(struct phy *_phy)
{
	sun55i_usb3_pcie_phy_power_set(_phy, false);

	return 0;
}

static const struct phy_ops sun55i_usb3_pcie_phy_ops = {
	.init		= sun55i_usb3_pcie_phy_init,
	.exit		= sun55i_usb3_pcie_phy_exit,
	.power_on	= sun55i_usb3_pcie_phy_power_on,
	.power_off	= sun55i_usb3_pcie_phy_power_off,
	.owner		= THIS_MODULE,
};

static int sun55i_usb3_pcie_phy_probe(struct platform_device *pdev)
{
	struct sun55i_usb3_pcie_phy *phy;
	struct device *dev = &pdev->dev;
	struct phy_provider *phy_provider;
	int ret;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	phy->dev = dev;
	phy->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(phy->clk)) {
		if (PTR_ERR(phy->clk) != -EPROBE_DEFER)
			dev_err(dev, "failed to get phy clock\n");
		return PTR_ERR(phy->clk);
	}

	phy->reset = devm_reset_control_get(dev, NULL);
	if (IS_ERR(phy->reset)) {
		dev_err(dev, "failed to get reset control\n");
		return PTR_ERR(phy->reset);
	}

	phy->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(phy->regs))
		return PTR_ERR(phy->regs);

	phy->regs_clk = phy->regs + PHY_CLK_OFFSET;
	if (IS_ERR(phy->regs_clk))
		return PTR_ERR(phy->regs_clk);

	phy->phy = devm_phy_create(dev, NULL, &sun55i_usb3_pcie_phy_ops);
	if (IS_ERR(phy->phy)) {
		dev_err(dev, "failed to create PHY\n");
		return PTR_ERR(phy->phy);
	}

	phy_set_drvdata(phy->phy, phy);
	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	ret = sun55i_usb3_pcie_clk_init(phy);
	if (ret)
		return ret;
	dev_info(phy->dev, "phy version is: 0x%x\n", readl(phy->regs));

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct of_device_id sun55i_usb3_pcie_phy_of_match[] = {
	{ .compatible = "allwinner,sun55i-a523-usb3-pcie-phy" },
	{ },
};
MODULE_DEVICE_TABLE(of, sun55i_usb3_pcie_phy_of_match);

static struct platform_driver sun55i_usb3_pcie_phy_driver = {
	.probe	= sun55i_usb3_pcie_phy_probe,
	.driver = {
		.of_match_table	= sun55i_usb3_pcie_phy_of_match,
		.name  = "sun55i-usb3-pcie-phy",
	}
};
module_platform_driver(sun55i_usb3_pcie_phy_driver);

MODULE_DESCRIPTION("Allwinner A523 USB3/PCIe phy driver");
MODULE_AUTHOR("Mikhail Kalashnikov <iuncuim@gmail.com>");
MODULE_LICENSE("GPL");
