// SPDX-License-Identifier: GPL-2.0
/*
 * ESWIN SATA PHY driver
 *
 * Copyright 2024, Beijing ESWIN Computing Technology Co., Ltd..
 * All rights reserved.
 *
 * Authors: Yulin Lu <luyulin@eswincomputing.com>
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

#define SATA_CLK_CTRL			0x0
#define SATA_AXI_LP_CTRL		0x08
#define SATA_MPLL_CTRL			0x20
#define SATA_PHY_CTRL0			0x28
#define SATA_PHY_CTRL1			0x2c
#define SATA_REF_CTRL1			0x38
#define SATA_REG_CTRL			0x34
#define SATA_LOS_IDEN			0x3c
#define SATA_RESET_CTRL			0x40
#define SATA_CLK_RST_SOURCE_PHY		BIT(0)
#define SATA_SYS_CLK_EN			BIT(28)
#define SATA_PHY_RESET			BIT(0)
#define SATA_PORT_RESET			BIT(1)
#define SATA_LOS_LEVEL			0x9
#define SATA_LOS_BIAS			(0x02 << 16)
#define SATA_REF_REPEATCLK_EN		BIT(0)
#define SATA_REF_USE_PAD		BIT(20)
#define SATA_P0_AMPLITUDE_GEN1		0x42
#define SATA_P0_AMPLITUDE_GEN2		(0x46 << 8)
#define SATA_P0_AMPLITUDE_GEN3		(0x73 << 16)
#define SATA_P0_PHY_TX_PREEMPH_GEN1	0x05
#define SATA_P0_PHY_TX_PREEMPH_GEN2	(0x05 << 8)
#define SATA_P0_PHY_TX_PREEMPH_GEN3	(0x08 << 16)
#define SATA_MPLL_MULTIPLIER		(0x3c << 16)
#define SATA_M_CSYSREQ			BIT(0)
#define SATA_S_CSYSREQ			BIT(16)
#define SATA_P0_PHY_STAT		0x24
#define SATA_P0_PHY_READY		BIT(0)

#define PHY_READY_TIMEOUT		(usecs_to_jiffies(4000))

struct eic7700_sata_phy {
	struct phy *phy;
	void __iomem *regs;
};

static int wait_for_phy_ready(void __iomem *base, u32 reg, u32 checkbit,
			      u32 status)
{
	unsigned long start = jiffies;
	unsigned long timeout = start + PHY_READY_TIMEOUT;

	while (time_before(start, timeout)) {
		if ((readl(base + reg) & checkbit) == status)
			return 0;
		usleep_range(50, 70);
	}

	return -EFAULT;
}

static int eic7700_sata_phy_init(struct phy *phy)
{
	struct eic7700_sata_phy *sata_phy = phy_get_drvdata(phy);
	u32 val = 0;
	int ret = 0;

	/*
	 * The SATA_CLK_CTRL register offset controls the pmalive, rxoob,
	 * and rbc clocks gate provided by the PHY through the HSP bus,
	 * and it is not registered in the clock tree.
	 */
	val = readl(sata_phy->regs + SATA_CLK_CTRL);
	val |= SATA_SYS_CLK_EN;
	writel(val, sata_phy->regs + SATA_CLK_CTRL);

	writel(SATA_CLK_RST_SOURCE_PHY, sata_phy->regs + SATA_REF_CTRL1);
	writel(SATA_P0_AMPLITUDE_GEN1 | SATA_P0_AMPLITUDE_GEN2 |
	       SATA_P0_AMPLITUDE_GEN3, sata_phy->regs + SATA_PHY_CTRL0);
	writel(SATA_P0_PHY_TX_PREEMPH_GEN1 | SATA_P0_PHY_TX_PREEMPH_GEN2 |
	       SATA_P0_PHY_TX_PREEMPH_GEN3, sata_phy->regs + SATA_PHY_CTRL1);
	writel(SATA_LOS_LEVEL | SATA_LOS_BIAS,
	       sata_phy->regs + SATA_LOS_IDEN);
	writel(SATA_M_CSYSREQ | SATA_S_CSYSREQ,
	       sata_phy->regs + SATA_AXI_LP_CTRL);
	writel(SATA_REF_REPEATCLK_EN | SATA_REF_USE_PAD,
	       sata_phy->regs + SATA_REG_CTRL);
	writel(SATA_MPLL_MULTIPLIER, sata_phy->regs + SATA_MPLL_CTRL);
	usleep_range(15, 20);

	/*
	 * The SATA_RESET_CTRL register offset controls reset/deassert
	 * for both the port and the PHY through the HSP bus,
	 * and it is not registered in the reset tree.
	 */
	val = readl(sata_phy->regs + SATA_RESET_CTRL);
	val &= ~(SATA_PHY_RESET | SATA_PORT_RESET);
	writel(val, sata_phy->regs + SATA_RESET_CTRL);

	ret = wait_for_phy_ready(sata_phy->regs, SATA_P0_PHY_STAT,
				 SATA_P0_PHY_READY, 1);
	if (ret < 0)
		dev_err(&sata_phy->phy->dev,
			"PHY READY check failed\n");
	return ret;
}

static int eic7700_sata_phy_exit(struct phy *phy)
{
	struct eic7700_sata_phy *sata_phy = phy_get_drvdata(phy);
	u32 val = 0;

	val = readl(sata_phy->regs + SATA_RESET_CTRL);
	val |= SATA_PHY_RESET | SATA_PORT_RESET;
	writel(val, sata_phy->regs + SATA_RESET_CTRL);

	val = readl(sata_phy->regs + SATA_CLK_CTRL);
	val &= ~SATA_SYS_CLK_EN;
	writel(val, sata_phy->regs + SATA_CLK_CTRL);

	return 0;
}

static const struct phy_ops eic7700_sata_phy_ops = {
	.init		= eic7700_sata_phy_init,
	.exit		= eic7700_sata_phy_exit,
	.owner		= THIS_MODULE,
};

static int eic7700_sata_phy_probe(struct platform_device *pdev)
{
	struct eic7700_sata_phy *sata_phy;
	struct device *dev = &pdev->dev;
	struct phy_provider *phy_provider;
	u32 val = 0;
	int ret = 0;

	sata_phy = devm_kzalloc(dev, sizeof(*sata_phy), GFP_KERNEL);
	if (!sata_phy)
		return -ENOMEM;

	sata_phy->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sata_phy->regs))
		return PTR_ERR(sata_phy->regs);

	dev_set_drvdata(dev, sata_phy);

	sata_phy->phy = devm_phy_create(dev, NULL, &eic7700_sata_phy_ops);
	if (IS_ERR(sata_phy->phy)) {
		dev_err(dev, "failed to create PHY\n");
		ret = PTR_ERR(sata_phy->phy);
		goto clk_disable;
	}

	phy_set_drvdata(sata_phy->phy, sata_phy);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(phy_provider)) {
		ret = PTR_ERR(phy_provider);
		goto clk_disable;
	}

	return 0;

clk_disable:
	val = readl(sata_phy->regs + SATA_CLK_CTRL);
	val &= ~SATA_SYS_CLK_EN;
	writel(val, sata_phy->regs + SATA_CLK_CTRL);

	return ret;
}

static const struct of_device_id eic7700_sata_phy_of_match[] = {
	{ .compatible = "eswin,eic7700-sata-phy" },
	{ },
};
MODULE_DEVICE_TABLE(of, eic7700_sata_phy_of_match);

static struct platform_driver eic7700_sata_phy_driver = {
	.probe	= eic7700_sata_phy_probe,
	.driver = {
		.of_match_table	= eic7700_sata_phy_of_match,
		.name  = "eswin,sata-phy",
		.suppress_bind_attrs = true,
	}
};
module_platform_driver(eic7700_sata_phy_driver);

MODULE_DESCRIPTION("SATA PHY driver for the ESWIN EIC7700 SoC");
MODULE_AUTHOR("Yulin Lu <luyulin@eswincomputing.com>");
MODULE_LICENSE("GPL");
