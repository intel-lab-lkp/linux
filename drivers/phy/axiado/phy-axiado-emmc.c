// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Axiado eMMC PHY driver
 *
 * Copyright (C) 2022-2025 Axiado Corporation (or its affiliates).
 *
 * Based on Arasan Driver (sdhci-pci-arasan.c)
 * sdhci-pci-arasan.c - Driver for Arasan PCI Controller with integrated phy.
 *
 * Copyright (C) 2017 Arasan Chip Systems Inc.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

/* Arasan eMMC 5.1 - PHY configuration registers */
#define CAP_REG_IN_S1_LSB		0x00
#define CAP_REG_IN_S1_MSB		0x04
#define PHY_CTRL_1			0x38
#define PHY_CTRL_2			0x3C
#define PHY_CTRL_3			0x40
#define STATUS				0x50

#define DLL_ENBL	BIT(26)
#define RTRIM_EN	BIT(21)
#define PDB_ENBL	BIT(23)
#define RETB_ENBL	BIT(1)

#define REN_STRB	BIT(27)
#define REN_CMD		BIT(12)
#define REN_DAT0	BIT(13)
#define REN_DAT1	BIT(14)
#define REN_DAT2	BIT(15)
#define REN_DAT3	BIT(16)
#define REN_DAT4	BIT(17)
#define REN_DAT5	BIT(18)
#define REN_DAT6	BIT(19)
#define REN_DAT7	BIT(20)
#define REN_CMD_EN	(REN_CMD | REN_DAT0 | REN_DAT1 | REN_DAT2 | \
		REN_DAT3 | REN_DAT4 | REN_DAT5 | REN_DAT6 | REN_DAT7)

/* Pull-UP Enable on CMD Line */
#define PU_CMD		BIT(3)
#define PU_DAT0		BIT(4)
#define PU_DAT1		BIT(5)
#define PU_DAT2		BIT(6)
#define PU_DAT3		BIT(7)
#define PU_DAT4		BIT(8)
#define PU_DAT5		BIT(9)
#define PU_DAT6		BIT(10)
#define PU_DAT7		BIT(11)
#define PU_CMD_EN (PU_CMD | PU_DAT0 | PU_DAT1 | PU_DAT2 | PU_DAT3 | \
		PU_DAT4 | PU_DAT5 | PU_DAT6 | PU_DAT7)

/* Slection value for the optimum delay from 1-32 output tap lines */
#define OTAP_DLY	0x02
/* DLL charge pump current trim default [1000] */
#define DLL_TRM_ICP	0x08
/* Select the frequency range of DLL Operation */
#define FRQ_SEL	0x01

#define OTAP_SEL(x)		(((x) << 7) | OTAPDLY_EN)
#define DLL_TRM(x)		(((x) << 22) | DLL_ENBL)
#define DLL_FRQSEL(x)	((x) << 25)

#define OTAPDLY_EN	BIT(11)

#define SEL_DLY_RXCLK	BIT(18)
#define SEL_DLY_TXCLK	BIT(19)

#define CALDONE_MASK	0x40
#define DLL_RDY_MASK	0x1
#define MAX_CLK_BUF0	BIT(20)
#define MAX_CLK_BUF1	BIT(21)
#define MAX_CLK_BUF2	BIT(22)

#define CLK_MULTIPLIER	0xC008E
#define LOOP_TIMEOUT	3000
#define TIMEOUT_DELAY	100

struct axiado_emmc_phy {
	void __iomem    *reg_base;
};

static void arasan_emmc_phy_write(struct axiado_emmc_phy *ax_phy, u32 offset, u32 data)
{
	writel(data, ax_phy->reg_base + offset);
}

static int arasan_emmc_phy_read(struct axiado_emmc_phy *ax_phy, u32 offset)
{
	u32 val = readl(ax_phy->reg_base + offset);

	return val;
}

static int axiado_emmc_phy_init(struct phy *phy)
{
	u32 val;
	ktime_t timeout;

	struct axiado_emmc_phy *ax_phy = phy_get_drvdata(phy);

	val = arasan_emmc_phy_read(ax_phy, PHY_CTRL_1);
	arasan_emmc_phy_write(ax_phy, PHY_CTRL_1, val | RETB_ENBL | RTRIM_EN);

	val = arasan_emmc_phy_read(ax_phy, PHY_CTRL_3);
	arasan_emmc_phy_write(ax_phy, PHY_CTRL_3, val | PDB_ENBL);

	/* Wait max 3000 ms */
	timeout = ktime_add_ms(ktime_get(), LOOP_TIMEOUT);

	while (1) {
		bool timedout = ktime_after(ktime_get(), timeout);

		if (arasan_emmc_phy_read(ax_phy, STATUS) & CALDONE_MASK)
			break;

		if (timedout) {
			dev_err(&phy->dev, "CALDONE_MASK bit is not cleared.");
			return -ETIMEDOUT;
		}
		udelay(TIMEOUT_DELAY);
	}

	val = arasan_emmc_phy_read(ax_phy, PHY_CTRL_1);

	arasan_emmc_phy_write(ax_phy, PHY_CTRL_1, val | REN_CMD_EN | PU_CMD_EN);

	val = arasan_emmc_phy_read(ax_phy, PHY_CTRL_2);
	arasan_emmc_phy_write(ax_phy, PHY_CTRL_2, val | REN_STRB);

	val = arasan_emmc_phy_read(ax_phy, PHY_CTRL_3);
	arasan_emmc_phy_write(ax_phy, PHY_CTRL_3, val | MAX_CLK_BUF0 |
			MAX_CLK_BUF1 | MAX_CLK_BUF2);

	val = arasan_emmc_phy_read(ax_phy, CAP_REG_IN_S1_MSB);
	arasan_emmc_phy_write(ax_phy, CAP_REG_IN_S1_MSB, CLK_MULTIPLIER);

	val = arasan_emmc_phy_read(ax_phy, PHY_CTRL_3);
	arasan_emmc_phy_write(ax_phy, PHY_CTRL_3, val | SEL_DLY_RXCLK |
			SEL_DLY_TXCLK);

	return 0;
}

static int axiado_emmc_phy_power_on(struct phy *phy)
{
	struct axiado_emmc_phy *ax_phy = phy_get_drvdata(phy);

	u32 val;
	ktime_t timeout;

	val = arasan_emmc_phy_read(ax_phy, PHY_CTRL_1);
	arasan_emmc_phy_write(ax_phy, PHY_CTRL_1, val | RETB_ENBL);

	val = arasan_emmc_phy_read(ax_phy, PHY_CTRL_3);
	arasan_emmc_phy_write(ax_phy, PHY_CTRL_3, val | PDB_ENBL);

	val = arasan_emmc_phy_read(ax_phy, PHY_CTRL_2);
	arasan_emmc_phy_write(ax_phy, PHY_CTRL_2, val | OTAP_SEL(OTAP_DLY));

	arasan_emmc_phy_read(ax_phy, PHY_CTRL_2);

	val = arasan_emmc_phy_read(ax_phy, PHY_CTRL_1);
	arasan_emmc_phy_write(ax_phy, PHY_CTRL_1, val | DLL_TRM(DLL_TRM_ICP));

	arasan_emmc_phy_write(ax_phy, STATUS, 0x00);

	val = arasan_emmc_phy_read(ax_phy, PHY_CTRL_3);
	arasan_emmc_phy_write(ax_phy, PHY_CTRL_3, val | DLL_FRQSEL(FRQ_SEL));

	/* Wait max 3000 ms */
	timeout = ktime_add_ms(ktime_get(), LOOP_TIMEOUT);

	while (1) {
		bool timedout = ktime_after(ktime_get(), timeout);

		if (arasan_emmc_phy_read(ax_phy, STATUS) & DLL_RDY_MASK)
			break;

		if (timedout) {
			dev_err(&phy->dev, "DLL_RDY_MASK bit is not cleared.");
			return -ETIMEDOUT;
		}
		udelay(TIMEOUT_DELAY);
	}
	return 0;
}

static const struct phy_ops axiado_emmc_phy_ops = {
	.init		= axiado_emmc_phy_init,
	.power_on	= axiado_emmc_phy_power_on,
	.owner		= THIS_MODULE,
};

static const struct of_device_id axiado_emmc_phy_of_match[] = {
	{ .compatible = "axiado,ax3000-emmc-phy"},
	{}
};
MODULE_DEVICE_TABLE(of, axiado_emmc_phy_of_match);

static int axiado_emmc_phy_probe(struct platform_device *pdev)
{
	struct axiado_emmc_phy *ax_phy;
	struct phy_provider *phy_provider;
	struct device *dev = &pdev->dev;
	const struct of_device_id *id;
	struct phy *generic_phy;
	struct resource *res;

	if (!dev->of_node)
		return -ENODEV;

	ax_phy = devm_kzalloc(dev, sizeof(*ax_phy), GFP_KERNEL);
	if (!ax_phy)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	ax_phy->reg_base = devm_ioremap_resource(&pdev->dev, res);

	if (IS_ERR(ax_phy->reg_base))
		return PTR_ERR(ax_phy->reg_base);

	id = of_match_node(axiado_emmc_phy_of_match, pdev->dev.of_node);
	if (!id) {
		dev_err(dev, "failed to get match_node\n");
		return -EINVAL;
	}

	generic_phy = devm_phy_create(dev, dev->of_node, &axiado_emmc_phy_ops);
	if (IS_ERR(generic_phy)) {
		dev_err(dev, "failed to create PHY\n");
		return PTR_ERR(generic_phy);
	}

	phy_set_drvdata(generic_phy, ax_phy);
	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static struct platform_driver axiado_emmc_phy_driver = {
	.probe		 = axiado_emmc_phy_probe,
	.driver		 = {
		.name	 = "axiado-emmc-phy",
		.of_match_table = axiado_emmc_phy_of_match,
	},
};
module_platform_driver(axiado_emmc_phy_driver);

MODULE_DESCRIPTION("AX3000 eMMC PHY Driver");
MODULE_AUTHOR("Axiado Corporation");
MODULE_LICENSE("GPL");
