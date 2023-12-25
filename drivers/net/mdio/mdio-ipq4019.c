// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright (c) 2015, The Linux Foundation. All rights reserved. */
/* Copyright (c) 2020 Sartura Ltd. */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/clk.h>

#define MDIO_MODE_REG				0x40
#define MDIO_ADDR_REG				0x44
#define MDIO_DATA_WRITE_REG			0x48
#define MDIO_DATA_READ_REG			0x4c
#define MDIO_CMD_REG				0x50
#define MDIO_CMD_ACCESS_BUSY		BIT(16)
#define MDIO_CMD_ACCESS_START		BIT(8)
#define MDIO_CMD_ACCESS_CODE_READ	0
#define MDIO_CMD_ACCESS_CODE_WRITE	1
#define MDIO_CMD_ACCESS_CODE_C45_ADDR	0
#define MDIO_CMD_ACCESS_CODE_C45_WRITE	1
#define MDIO_CMD_ACCESS_CODE_C45_READ	2

/* 0 = Clause 22, 1 = Clause 45 */
#define MDIO_MODE_C45				BIT(8)

/* MDC frequency is SYS_CLK/(MDIO_CLK_DIV + 1), SYS_CLK is 100MHz */
#define MDIO_CLK_DIV_MASK			GENMASK(7, 0)

#define IPQ4019_MDIO_TIMEOUT	10000
#define IPQ4019_MDIO_SLEEP		10

/* MDIO clock source frequency is fixed to 100M */
#define IPQ_MDIO_CLK_RATE	100000000

/* SoC UNIPHY fixed clock */
#define IPQ_UNIPHY_AHB_CLK_RATE	100000000
#define IPQ_UNIPHY_SYS_CLK_RATE	24000000

#define IPQ_PHY_SET_DELAY_US	100000

/* Maximum SOC PCS(uniphy) number on IPQ platform */
#define ETH_LDO_RDY_CNT				3

#define CMN_PLL_REFERENCE_SOURCE_SEL		0x28
#define CMN_PLL_REFCLK_SOURCE_DIV		GENMASK(9, 8)

#define CMN_PLL_REFERENCE_CLOCK			0x784
#define CMN_PLL_REFCLK_EXTERNAL			BIT(9)
#define CMN_PLL_REFCLK_DIV			GENMASK(8, 4)
#define CMN_PLL_REFCLK_INDEX			GENMASK(3, 0)

#define CMN_PLL_POWER_ON_AND_RESET		0x780
#define CMN_ANA_EN_SW_RSTN			BIT(6)

enum mdio_clk_id {
	MDIO_CLK_MDIO_AHB,
	MDIO_CLK_UNIPHY0_AHB,
	MDIO_CLK_UNIPHY0_SYS,
	MDIO_CLK_UNIPHY1_AHB,
	MDIO_CLK_UNIPHY1_SYS,
	MDIO_CLK_CNT
};

struct ipq4019_mdio_data {
	void __iomem *membase;
	void __iomem *cmn_membase;
	void __iomem *eth_ldo_rdy[ETH_LDO_RDY_CNT];
	struct clk *clk[MDIO_CLK_CNT];
	int clk_div;
};

static const char *const mdio_clk_name[] = {
	"gcc_mdio_ahb_clk",
	"uniphy0_ahb",
	"uniphy0_sys",
	"uniphy1_ahb",
	"uniphy1_sys"
};

static int ipq4019_mdio_wait_busy(struct mii_bus *bus)
{
	struct ipq4019_mdio_data *priv = bus->priv;
	unsigned int busy;

	return readl_poll_timeout(priv->membase + MDIO_CMD_REG, busy,
				  (busy & MDIO_CMD_ACCESS_BUSY) == 0,
				  IPQ4019_MDIO_SLEEP, IPQ4019_MDIO_TIMEOUT);
}

static int ipq4019_mdio_read_c45(struct mii_bus *bus, int mii_id, int mmd,
				 int reg)
{
	struct ipq4019_mdio_data *priv = bus->priv;
	unsigned int data;
	unsigned int cmd;

	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	data = readl(priv->membase + MDIO_MODE_REG);

	data |= MDIO_MODE_C45;
	data |= FIELD_PREP(MDIO_CLK_DIV_MASK, priv->clk_div);

	writel(data, priv->membase + MDIO_MODE_REG);

	/* issue the phy address and mmd */
	writel((mii_id << 8) | mmd, priv->membase + MDIO_ADDR_REG);

	/* issue reg */
	writel(reg, priv->membase + MDIO_DATA_WRITE_REG);

	cmd = MDIO_CMD_ACCESS_START | MDIO_CMD_ACCESS_CODE_C45_ADDR;

	/* issue read command */
	writel(cmd, priv->membase + MDIO_CMD_REG);

	/* Wait read complete */
	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	cmd = MDIO_CMD_ACCESS_START | MDIO_CMD_ACCESS_CODE_C45_READ;

	writel(cmd, priv->membase + MDIO_CMD_REG);

	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	/* Read and return data */
	return readl(priv->membase + MDIO_DATA_READ_REG);
}

static int ipq4019_mdio_read_c22(struct mii_bus *bus, int mii_id, int regnum)
{
	struct ipq4019_mdio_data *priv = bus->priv;
	unsigned int data;
	unsigned int cmd;

	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	data = readl(priv->membase + MDIO_MODE_REG);

	data &= ~MDIO_MODE_C45;
	data |= FIELD_PREP(MDIO_CLK_DIV_MASK, priv->clk_div);

	writel(data, priv->membase + MDIO_MODE_REG);

	/* issue the phy address and reg */
	writel((mii_id << 8) | regnum, priv->membase + MDIO_ADDR_REG);

	cmd = MDIO_CMD_ACCESS_START | MDIO_CMD_ACCESS_CODE_READ;

	/* issue read command */
	writel(cmd, priv->membase + MDIO_CMD_REG);

	/* Wait read complete */
	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	/* Read and return data */
	return readl(priv->membase + MDIO_DATA_READ_REG);
}

static int ipq4019_mdio_write_c45(struct mii_bus *bus, int mii_id, int mmd,
				  int reg, u16 value)
{
	struct ipq4019_mdio_data *priv = bus->priv;
	unsigned int data;
	unsigned int cmd;

	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	data = readl(priv->membase + MDIO_MODE_REG);

	data |= MDIO_MODE_C45;
	data |= FIELD_PREP(MDIO_CLK_DIV_MASK, priv->clk_div);

	writel(data, priv->membase + MDIO_MODE_REG);

	/* issue the phy address and mmd */
	writel((mii_id << 8) | mmd, priv->membase + MDIO_ADDR_REG);

	/* issue reg */
	writel(reg, priv->membase + MDIO_DATA_WRITE_REG);

	cmd = MDIO_CMD_ACCESS_START | MDIO_CMD_ACCESS_CODE_C45_ADDR;

	writel(cmd, priv->membase + MDIO_CMD_REG);

	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	/* issue write data */
	writel(value, priv->membase + MDIO_DATA_WRITE_REG);

	cmd = MDIO_CMD_ACCESS_START | MDIO_CMD_ACCESS_CODE_C45_WRITE;
	writel(cmd, priv->membase + MDIO_CMD_REG);

	/* Wait write complete */
	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	return 0;
}

static int ipq4019_mdio_write_c22(struct mii_bus *bus, int mii_id, int regnum,
				  u16 value)
{
	struct ipq4019_mdio_data *priv = bus->priv;
	unsigned int data;
	unsigned int cmd;

	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	/* Enter Clause 22 mode */
	data = readl(priv->membase + MDIO_MODE_REG);

	data &= ~MDIO_MODE_C45;
	data |= FIELD_PREP(MDIO_CLK_DIV_MASK, priv->clk_div);

	writel(data, priv->membase + MDIO_MODE_REG);

	/* issue the phy address and reg */
	writel((mii_id << 8) | regnum, priv->membase + MDIO_ADDR_REG);

	/* issue write data */
	writel(value, priv->membase + MDIO_DATA_WRITE_REG);

	/* issue write command */
	cmd = MDIO_CMD_ACCESS_START | MDIO_CMD_ACCESS_CODE_WRITE;

	writel(cmd, priv->membase + MDIO_CMD_REG);

	/* Wait write complete */
	if (ipq4019_mdio_wait_busy(bus))
		return -ETIMEDOUT;

	return 0;
}

/* For the CMN PLL block, the reference clock can be configured according to
 * the device tree property "qcom,cmn-ref-clock-frequency", the internal 48MHZ
 * is used by default.
 *
 * The output clock of CMN PLL block is provided to the ethernet devices,
 * threre are 4 CMN PLL output clocks (1*25MHZ + 3*50MHZ) enabled by default.
 *
 * Such as the output 50M clock for the qca8084 ethernet PHY.
 */
static int ipq_cmn_clock_config(struct mii_bus *bus)
{
	struct ipq4019_mdio_data *priv;
	u32 reg_val, src_sel, ref_clk;
	int ret;

	priv = bus->priv;
	if (priv->cmn_membase) {
		reg_val = readl(priv->cmn_membase + CMN_PLL_REFERENCE_CLOCK);

		/* Select reference clock source of CMN PLL block, which can
		 * be from wifi module or the external xtal.
		 *
		 * If absent, the wifi internal 48MHz is used as the reference
		 * clock source of CMN PLL block, if the 48MHZ is specified,
		 * which means the xtal 48MHZ is selected.
		 */
		ret = of_property_read_u32(bus->parent->of_node,
					   "qcom,cmn-ref-clock-frequency",
					   &ref_clk);
		if (!ret) {
			switch (ref_clk) {
			case 25000000:
				reg_val &= ~(CMN_PLL_REFCLK_EXTERNAL |
					     CMN_PLL_REFCLK_INDEX);
				reg_val |= (CMN_PLL_REFCLK_EXTERNAL |
					    FIELD_PREP(CMN_PLL_REFCLK_INDEX, 3));
				break;
			case 31250000:
				reg_val &= ~(CMN_PLL_REFCLK_EXTERNAL |
					     CMN_PLL_REFCLK_INDEX);
				reg_val |= (CMN_PLL_REFCLK_EXTERNAL |
					    FIELD_PREP(CMN_PLL_REFCLK_INDEX, 4));
				break;
			case 40000000:
				reg_val &= ~(CMN_PLL_REFCLK_EXTERNAL |
					     CMN_PLL_REFCLK_INDEX);
				reg_val |= (CMN_PLL_REFCLK_EXTERNAL |
					    FIELD_PREP(CMN_PLL_REFCLK_INDEX, 6));
				break;
			case 48000000:
				reg_val &= ~(CMN_PLL_REFCLK_EXTERNAL |
					     CMN_PLL_REFCLK_INDEX);
				reg_val |= (CMN_PLL_REFCLK_EXTERNAL |
					    FIELD_PREP(CMN_PLL_REFCLK_INDEX, 7));
				break;
			case 50000000:
				reg_val &= ~(CMN_PLL_REFCLK_EXTERNAL |
					     CMN_PLL_REFCLK_INDEX);
				reg_val |= (CMN_PLL_REFCLK_EXTERNAL |
					    FIELD_PREP(CMN_PLL_REFCLK_INDEX, 8));
				break;
			case 96000000:
				src_sel = readl(priv->cmn_membase +
						CMN_PLL_REFERENCE_SOURCE_SEL);
				src_sel &= ~CMN_PLL_REFCLK_SOURCE_DIV;
				src_sel |= FIELD_PREP(CMN_PLL_REFCLK_SOURCE_DIV, 0);
				writel(src_sel, priv->cmn_membase +
				       CMN_PLL_REFERENCE_SOURCE_SEL);

				reg_val &= ~CMN_PLL_REFCLK_DIV;
				reg_val |= FIELD_PREP(CMN_PLL_REFCLK_DIV, 2);
				break;
			default:
				return -EINVAL;
			}
		} else if (ret == -EINVAL) {
			/* the internal 48MHZ is selected by default. */
			reg_val &= ~(CMN_PLL_REFCLK_EXTERNAL | CMN_PLL_REFCLK_INDEX);
			reg_val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 7);
		} else {
			return ret;
		}

		writel(reg_val, priv->cmn_membase + CMN_PLL_REFERENCE_CLOCK);

		/* assert CMN PLL */
		reg_val = readl(priv->cmn_membase + CMN_PLL_POWER_ON_AND_RESET);
		reg_val &= ~CMN_ANA_EN_SW_RSTN;
		writel(reg_val, priv->cmn_membase);
		fsleep(IPQ_PHY_SET_DELAY_US);

		/* deassert CMN PLL */
		reg_val |= CMN_ANA_EN_SW_RSTN;
		writel(reg_val, priv->cmn_membase + CMN_PLL_POWER_ON_AND_RESET);
		fsleep(IPQ_PHY_SET_DELAY_US);
	}

	return 0;
}

static int ipq_mdio_reset(struct mii_bus *bus)
{
	struct ipq4019_mdio_data *priv = bus->priv;
	unsigned long rate;
	int ret, index;

	ret = ipq_cmn_clock_config(bus);
	if (ret)
		return ret;

	/* For the platform ipq5332, there are two SoC uniphies available
	 * for connecting with ethernet PHY, the SoC uniphy gcc clock
	 * should be enabled for resetting the connected device such
	 * as qca8386 switch, qca8081 PHY or other PHYs effectively.
	 *
	 * Configure MDIO/UNIPHY clock source frequency if clock instance
	 * is specified in the device tree.
	 */
	for (index = MDIO_CLK_MDIO_AHB; index < MDIO_CLK_CNT; index++) {
		switch (index) {
		case MDIO_CLK_MDIO_AHB:
			rate = IPQ_MDIO_CLK_RATE;
			break;
		case MDIO_CLK_UNIPHY0_AHB:
		case MDIO_CLK_UNIPHY1_AHB:
			rate = IPQ_UNIPHY_AHB_CLK_RATE;
			break;
		case MDIO_CLK_UNIPHY0_SYS:
		case MDIO_CLK_UNIPHY1_SYS:
			rate = IPQ_UNIPHY_SYS_CLK_RATE;
			break;
		default:
			break;
		}

		ret = clk_set_rate(priv->clk[index], rate);
		if (ret)
			return ret;

		ret = clk_prepare_enable(priv->clk[index]);
		if (ret)
			return ret;
	}

	if (ret == 0)
		mdelay(10);

	return ret;
}

static int ipq_mdio_clk_set(struct platform_device *pdev, int *clk_div)
{
	int freq;

	/* Keep the MDIO clock divider as the hardware default value 0xff if
	 * the MDIO property "clock-frequency" is not specified.
	 */
	if (of_property_read_u32(pdev->dev.of_node, "clock-frequency", &freq)) {
		*clk_div = 0xff;
		return 0;
	}

	/* MDC frequency is SYS_CLK/(MDIO_CLK_DIV + 1), SYS_CLK is fixed
	 * to 100MHz, the MDIO_CLK_DIV can be only configured the valid
	 * values, other values cause malfunction.
	 */
	switch (freq) {
	case 390625:
	case 781250:
	case 1562500:
	case 3125000:
	case 6250000:
	case 12500000:
		*clk_div = DIV_ROUND_UP(IPQ_MDIO_CLK_RATE, freq) - 1;
		break;
	default:
		dev_err(&pdev->dev, "Invalid clock frequency %dHZ\n", freq);
		return -EINVAL;
	}

	return 0;
}

static int ipq4019_mdio_probe(struct platform_device *pdev)
{
	struct ipq4019_mdio_data *priv;
	struct mii_bus *bus;
	struct resource *res;
	int ret, index;

	bus = devm_mdiobus_alloc_size(&pdev->dev, sizeof(*priv));
	if (!bus)
		return -ENOMEM;

	priv = bus->priv;

	priv->membase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->membase))
		return PTR_ERR(priv->membase);

	/* These platform resources are provided on the chipset IPQ5018 or
	 * IPQ5332.
	 */
	/* This resource are optional */
	for (index = 0; index < ETH_LDO_RDY_CNT; index++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, index + 1);
		if (res && strcmp(res->name, "cmn_blk")) {
			priv->eth_ldo_rdy[index] = devm_ioremap(&pdev->dev,
								res->start,
								resource_size(res));

			/* The ethernet LDO enable is necessary to reset PHY
			 * by GPIO, some PHY(such as qca8084) GPIO reset uses
			 * the MDIO level reset, so this function should be
			 * called before the MDIO bus register.
			 */
			if (priv->eth_ldo_rdy[index]) {
				u32 val;

				val = readl(priv->eth_ldo_rdy[index]);
				val |= BIT(0);
				writel(val, priv->eth_ldo_rdy[index]);
				fsleep(IPQ_PHY_SET_DELAY_US);
			}
		}
	}

	/* The CMN block resource is for providing clock source to ethernet,
	 * which can be optionally configured on the platform ipq9574 and
	 * ipq5332.
	 */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cmn_blk");
	if (res) {
		priv->cmn_membase = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(priv->cmn_membase))
			return PTR_ERR(priv->cmn_membase);
	}

	for (index = 0; index < MDIO_CLK_CNT; index++) {
		priv->clk[index] = devm_clk_get_optional(&pdev->dev,
							 mdio_clk_name[index]);
		if (IS_ERR(priv->clk[index]))
			return PTR_ERR(priv->clk[index]);
	}

	ret = ipq_mdio_clk_set(pdev, &priv->clk_div);
	if (ret)
		return ret;

	bus->name = "ipq4019_mdio";
	bus->read = ipq4019_mdio_read_c22;
	bus->write = ipq4019_mdio_write_c22;
	bus->read_c45 = ipq4019_mdio_read_c45;
	bus->write_c45 = ipq4019_mdio_write_c45;
	bus->reset = ipq_mdio_reset;
	bus->parent = &pdev->dev;
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s%d", pdev->name, pdev->id);

	ret = of_mdiobus_register(bus, pdev->dev.of_node);
	if (ret) {
		dev_err(&pdev->dev, "Cannot register MDIO bus!\n");
		return ret;
	}

	platform_set_drvdata(pdev, bus);

	return 0;
}

static void ipq4019_mdio_remove(struct platform_device *pdev)
{
	struct mii_bus *bus = platform_get_drvdata(pdev);

	mdiobus_unregister(bus);
}

static const struct of_device_id ipq4019_mdio_dt_ids[] = {
	{ .compatible = "qcom,ipq4019-mdio" },
	{ .compatible = "qcom,ipq5018-mdio" },
	{ }
};
MODULE_DEVICE_TABLE(of, ipq4019_mdio_dt_ids);

static struct platform_driver ipq4019_mdio_driver = {
	.probe = ipq4019_mdio_probe,
	.remove_new = ipq4019_mdio_remove,
	.driver = {
		.name = "ipq4019-mdio",
		.of_match_table = ipq4019_mdio_dt_ids,
	},
};

module_platform_driver(ipq4019_mdio_driver);

MODULE_DESCRIPTION("ipq4019 MDIO interface driver");
MODULE_AUTHOR("Qualcomm Atheros");
MODULE_LICENSE("Dual BSD/GPL");
