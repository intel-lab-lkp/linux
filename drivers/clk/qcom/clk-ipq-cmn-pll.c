// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/*
 * Common PLL block expects the reference clock from on-board Wi-Fi block,
 * and supplies the fixed rate clocks as output to the Ethernet hardware
 * blocks. The Ethernet related blocks include PPE (packet process engine)
 * and the external connected PHY (or switch) chip receiving clocks from
 * the common PLL.
 *
 * On the IPQ9574 SoC, There are three clocks with 50 MHZ, one clock with
 * 25 MHZ which are output from the common PLL to Ethernet PHY (or switch),
 * and one clock with 353 MHZ to PPE.
 *
 *               +---------+
 *               |   GCC   |
 *               +--+---+--+
 *           AHB CLK|   |SYS CLK
 *                  V   V
 *          +-------+---+------+
 *          |                  +-------------> eth0-50mhz
 * REF CLK  |     IPQ9574      |
 * -------->+                  +-------------> eth1-50mhz
 *          |  CMN PLL block   |
 *          |                  +-------------> eth2-50mhz
 *          |                  |
 *          +---------+--------+-------------> eth-25mhz
 *                    |
 *                    V
 *                    ppe-353mhz
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define CMN_PLL_REFCLK_SRC_SELECTION		0x28
#define CMN_PLL_REFCLK_SRC_DIV			GENMASK(9, 8)

#define CMN_PLL_REFCLK_CONFIG			0x784
#define CMN_PLL_REFCLK_EXTERNAL			BIT(9)
#define CMN_PLL_REFCLK_DIV			GENMASK(8, 4)
#define CMN_PLL_REFCLK_INDEX			GENMASK(3, 0)

#define CMN_PLL_POWER_ON_AND_RESET		0x780
#define CMN_ANA_EN_SW_RSTN			BIT(6)

/**
 * struct cmn_pll_fixed_clk - Common PLL output clocks information
 * @nrates:	Number of elements in rates
 * @rates:	Array of clock rates supplied by common PLL
 */
struct cmn_pll_fixed_clk {
	int nrates;
	const unsigned long *rates;
};

/*
 * The clock rates are for the output clock ppe-353mhz, eth0-50mhz
 * eth1-50mhz, eth2-50mhz and eth-25mhz.
 */
static const unsigned long ipq9574_rates[] = {
	353000000UL, 50000000UL, 50000000UL, 50000000UL, 25000000UL,
};

static const struct cmn_pll_fixed_clk ipq9574_fixed_clk = {
	.nrates = ARRAY_SIZE(ipq9574_rates),
	.rates = ipq9574_rates,
};

static int ipq_cmn_pll_config(struct device *dev, unsigned long parent_rate)
{
	void __iomem *base;
	u32 val;

	base = devm_of_iomap(dev, dev->of_node, 0, NULL);
	if (IS_ERR(base))
		return PTR_ERR(base);

	val = readl(base + CMN_PLL_REFCLK_CONFIG);
	val &= ~(CMN_PLL_REFCLK_EXTERNAL | CMN_PLL_REFCLK_INDEX);

	/*
	 * Configure the reference input clock selection as per the given rate.
	 * The output clock rates are always of fixed value.
	 */
	switch (parent_rate) {
	case 25000000:
		val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 3);
		break;
	case 31250000:
		val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 4);
		break;
	case 40000000:
		val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 6);
		break;
	case 48000000:
		val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 7);
		break;
	case 50000000:
		val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 8);
		break;
	case 96000000:
		val |= FIELD_PREP(CMN_PLL_REFCLK_INDEX, 7);
		val &= ~CMN_PLL_REFCLK_DIV;
		val |= FIELD_PREP(CMN_PLL_REFCLK_DIV, 2);
		break;
	default:
		return -EINVAL;
	}

	writel(val, base + CMN_PLL_REFCLK_CONFIG);

	/* Update the source clock rate selection. Only 96 MHZ uses 0. */
	val = readl(base + CMN_PLL_REFCLK_SRC_SELECTION);
	val &= ~CMN_PLL_REFCLK_SRC_DIV;
	if (parent_rate != 96000000)
		val |= FIELD_PREP(CMN_PLL_REFCLK_SRC_DIV, 1);

	writel(val, base + CMN_PLL_REFCLK_SRC_SELECTION);

	/*
	 * Reset the common PLL block by asserting/de-asserting for 100 ms
	 * each, to ensure the updated configurations take effect.
	 */
	val = readl(base + CMN_PLL_POWER_ON_AND_RESET);
	val &= ~CMN_ANA_EN_SW_RSTN;
	writel(val, base);
	msleep(100);

	val |= CMN_ANA_EN_SW_RSTN;
	writel(val, base + CMN_PLL_POWER_ON_AND_RESET);
	msleep(100);

	return 0;
}

static int ipq_cmn_pll_clk_register(struct device *dev, const char *parent)
{
	const struct cmn_pll_fixed_clk *fixed_clk;
	struct clk_hw_onecell_data *data;
	const char *clk_name;
	struct clk_hw *hw;
	int index;

	fixed_clk = of_device_get_match_data(dev);
	if (!fixed_clk)
		return -ENODEV;

	data = devm_kzalloc(dev, struct_size(data, hws, fixed_clk->nrates),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	/*
	 * Register the fixed rate output clocks with the correct clock names,
	 * the number of clocks and clock names are guaranteed by DTS.
	 */
	for (index = 0; index < fixed_clk->nrates; index++) {
		if (of_property_read_string_index(dev->of_node,
						  "clock-output-names",
						  index, &clk_name))
			return -ENODEV;

		hw = devm_clk_hw_register_fixed_rate(dev, clk_name, parent, 0,
						     fixed_clk->rates[index]);
		if (IS_ERR(hw))
			return PTR_ERR(hw);

		data->hws[index] = hw;
	}
	data->num = fixed_clk->nrates;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
}

static int ipq_cmn_pll_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk *clk;
	int ret;

	/*
	 * To access the common PLL registers, the GCC AHB & SYSY clocks
	 * for common PLL block need to be enabled.
	 */
	clk = devm_clk_get_enabled(dev, "ahb");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Enable AHB clock failed\n");

	clk = devm_clk_get_enabled(dev, "sys");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Enable SYS clock failed\n");

	clk = devm_clk_get(dev, "ref");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Get reference clock failed\n");

	/* Configure common PLL to apply the reference clock. */
	ret = ipq_cmn_pll_config(dev, clk_get_rate(clk));
	if (ret)
		return dev_err_probe(dev, ret, "Configure common PLL failed\n");

	return ipq_cmn_pll_clk_register(dev, __clk_get_name(clk));
}

static const struct of_device_id ipq_cmn_pll_clk_ids[] = {
	{ .compatible = "qcom,ipq9574-cmn-pll", .data = &ipq9574_fixed_clk },
	{ }
};

static struct platform_driver ipq_cmn_pll_clk_driver = {
	.probe = ipq_cmn_pll_clk_probe,
	.driver = {
		.name = "ipq_cmn_pll",
		.of_match_table = ipq_cmn_pll_clk_ids,
	},
};

module_platform_driver(ipq_cmn_pll_clk_driver);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. IPQ Common PLL Driver");
MODULE_LICENSE("GPL");
