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
#include <linux/gpio/consumer.h>

#define MDIO_MODE_REG				0x40
#define MDIO_ADDR_REG				0x44
#define MDIO_DATA_WRITE_REG			0x48
#define MDIO_DATA_READ_REG			0x4c
#define MDIO_CMD_REG				0x50
#define MDIO_CMD_ACCESS_BUSY			BIT(16)
#define MDIO_CMD_ACCESS_START			BIT(8)
#define MDIO_CMD_ACCESS_CODE_READ		0
#define MDIO_CMD_ACCESS_CODE_WRITE		1
#define MDIO_CMD_ACCESS_CODE_C45_ADDR		0
#define MDIO_CMD_ACCESS_CODE_C45_WRITE		1
#define MDIO_CMD_ACCESS_CODE_C45_READ		2

/* 0 = Clause 22, 1 = Clause 45 */
#define MDIO_MODE_C45				BIT(8)

/* MDC frequency is SYS_CLK/(MDIO_CLK_DIV + 1), SYS_CLK is 100MHz */
#define MDIO_CLK_DIV_MASK			GENMASK(7, 0)

#define IPQ4019_MDIO_TIMEOUT			10000
#define IPQ4019_MDIO_SLEEP			10

/* MDIO clock source frequency is fixed to 100M */
#define IPQ_MDIO_CLK_RATE			100000000
#define IPQ_UNIPHY_AHB_CLK_RATE			100000000
#define IPQ_UNIPHY_SYS_CLK_RATE			24000000

#define IPQ_PHY_SET_DELAY_US			100000

/* Maximum SOC PCS(uniphy) number on IPQ platform */
#define ETH_LDO_RDY_CNT				3

#define CMN_PLL_REFERENCE_CLOCK			0x784
#define CMN_PLL_REFCLK_INDEX			GENMASK(3, 0)
#define CMN_PLL_REFCLK_EXTERNAL			BIT(9)

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
	struct gpio_descs *reset_gpios;
	int clk_div;
};

const char *const mdio_clk_name[] = {
	"gcc_mdio_ahb_clk",
	"gcc_uniphy0_ahb_clk",
	"gcc_uniphy0_sys_clk",
	"gcc_uniphy1_ahb_clk",
	"gcc_uniphy1_sys_clk"
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
 * the device tree property "cmn_ref_clk", the internal 48MHZ is used by default
 * on the ipq533 platform.
 *
 * The output clock of CMN PLL block is provided to the MDIO slave devices,
 * threre are 4 CMN PLL output clocks (1x25MHZ + 3x50MHZ) enabled by default.
 *
 * such as the output 50M clock for the qca8084 PHY.
 */
static void ipq_cmn_clock_config(struct mii_bus *bus)
{
	u32 reg_val;
	const char *cmn_ref_clk;
	struct ipq4019_mdio_data *priv = bus->priv;

	if (priv && priv->cmn_membase) {
		reg_val = readl(priv->cmn_membase + CMN_PLL_REFERENCE_CLOCK);
		reg_val &= ~(CMN_PLL_REFCLK_EXTERNAL | CMN_PLL_REFCLK_INDEX);

		/* Select reference clock source */
		cmn_ref_clk = of_get_property(bus->parent->of_node, "cmn_ref_clk", NULL);
		if (!cmn_ref_clk) {
			/* Internal 48MHZ selected by default */
			reg_val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 7);
		} else {
			if (!strcmp(cmn_ref_clk, "external_25MHz"))
				reg_val |= (CMN_PLL_REFCLK_EXTERNAL |
					    FIELD_PREP(CMN_PLL_REFCLK_INDEX, 3));
			else if (!strcmp(cmn_ref_clk, "external_31250KHz"))
				reg_val |= (CMN_PLL_REFCLK_EXTERNAL |
					    FIELD_PREP(CMN_PLL_REFCLK_INDEX, 4));
			else if (!strcmp(cmn_ref_clk, "external_40MHz"))
				reg_val |= (CMN_PLL_REFCLK_EXTERNAL |
					    FIELD_PREP(CMN_PLL_REFCLK_INDEX, 6));
			else if (!strcmp(cmn_ref_clk, "external_48MHz"))
				reg_val |= (CMN_PLL_REFCLK_EXTERNAL |
					    FIELD_PREP(CMN_PLL_REFCLK_INDEX, 7));
			else if (!strcmp(cmn_ref_clk, "external_50MHz"))
				reg_val |= (CMN_PLL_REFCLK_EXTERNAL |
					    FIELD_PREP(CMN_PLL_REFCLK_INDEX, 8));
			else
				reg_val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 7);
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
}

static int ipq_mdio_reset(struct mii_bus *bus)
{
	struct ipq4019_mdio_data *priv = bus->priv;
	u32 val;
	int ret;

	ipq_cmn_clock_config(bus);

	/* For the platform ipq5332, there are two uniphy available to connect the
	 * ethernet devices, the uniphy gcc clock should be enabled for resetting
	 * the connected device such as qca8386 switch or qca8081 PHY effectively.
	 */
	if (of_device_is_compatible(bus->parent->of_node, "qcom,ipq5332-mdio")) {
		int i;
		unsigned long rate = 0;

		for (i = MDIO_CLK_UNIPHY0_AHB; i < MDIO_CLK_CNT; i++) {
			switch (i) {
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

			ret = clk_set_rate(priv->clk[i], rate);
			if (ret)
				return ret;

			ret = clk_prepare_enable(priv->clk[i]);
			if (ret)
				return ret;
		}
	}

	/* To indicate CMN_PLL that ethernet_ldo has been ready if platform resource 1
	 * or more resource are specified in the device tree.
	 */
	for (ret = 0; ret < ETH_LDO_RDY_CNT; ret++) {
		if (priv->eth_ldo_rdy[ret]) {
			val = readl(priv->eth_ldo_rdy[ret]);
			val |= BIT(0);
			writel(val, priv->eth_ldo_rdy[ret]);
			fsleep(IPQ_PHY_SET_DELAY_US);
		}
	}

	/* Do the optional reset on the devices connected with MDIO bus */
	if (priv->reset_gpios) {
		unsigned long *values = bitmap_zalloc(priv->reset_gpios->ndescs, GFP_KERNEL);

		if (!values)
			return -ENOMEM;

		bitmap_fill(values, priv->reset_gpios->ndescs);
		gpiod_set_array_value_cansleep(priv->reset_gpios->ndescs, priv->reset_gpios->desc,
					       priv->reset_gpios->info, values);

		fsleep(IPQ_PHY_SET_DELAY_US);
		bitmap_zero(values, priv->reset_gpios->ndescs);
		gpiod_set_array_value_cansleep(priv->reset_gpios->ndescs, priv->reset_gpios->desc,
					       priv->reset_gpios->info, values);
		bitmap_free(values);
	}

	/* Configure MDIO clock source frequency if clock is specified in the device tree */
	ret = clk_set_rate(priv->clk[MDIO_CLK_MDIO_AHB], IPQ_MDIO_CLK_RATE);
	if (ret)
		return ret;

	ret = clk_prepare_enable(priv->clk[MDIO_CLK_MDIO_AHB]);
	if (ret == 0)
		mdelay(10);

	return ret;
}

static int ipq4019_mdio_probe(struct platform_device *pdev)
{
	struct ipq4019_mdio_data *priv;
	struct mii_bus *bus;
	struct resource *res;
	int ret;

	bus = devm_mdiobus_alloc_size(&pdev->dev, sizeof(*priv));
	if (!bus)
		return -ENOMEM;

	priv = bus->priv;

	priv->membase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->membase))
		return PTR_ERR(priv->membase);

	/* The platform resource is provided on the chipset IPQ5018/IPQ5332 */
	/* This resource is optional */
	for (ret = 0; ret < ETH_LDO_RDY_CNT; ret++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, ret + 1);
		if (res && strcmp(res->name, "cmn_blk"))
			priv->eth_ldo_rdy[ret] = devm_ioremap(&pdev->dev,
							      res->start, resource_size(res));
	}

	/* The CMN block resource is for providing clock source of ethernet, which can
	 * be optionally configured on the platform ipq9574 and ipq5332.
	 */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cmn_blk");
	if (res) {
		priv->cmn_membase = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(priv->cmn_membase))
			return PTR_ERR(priv->cmn_membase);
	}

	for (ret = 0; ret < MDIO_CLK_CNT; ret++) {
		priv->clk[ret] = devm_clk_get_optional(&pdev->dev, mdio_clk_name[ret]);
		if (IS_ERR(priv->clk[ret]))
			return PTR_ERR(priv->clk[ret]);
	}

	/* This GPIO reset is for qca8084 PHY, which is only probeable by MDIO bus
	 * after the following steps completed.
	 *
	 * 1. Enable LDO to provide clock for qca8084 and enable SoC GCC uniphy related clocks.
	 * 2. Do GPIO reset on the qca8084 PHY.
	 * 3. Configure the PHY address that is customized according to device treee.
	 * 4. Configure the related qca8084 GCC clock & reset.
	 */
	priv->reset_gpios = devm_gpiod_get_array_optional(&pdev->dev, "phy-reset", GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset_gpios))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->reset_gpios),
				     "mii_bus %s couldn't get reset GPIO\n", bus->id);

	/* MDIO default frequency is 6.25MHz */
	priv->clk_div = 0xf;

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
	{ .compatible = "qcom,ipq5332-mdio" },
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
