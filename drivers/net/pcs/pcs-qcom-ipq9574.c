// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/net/qcom,ipq9574-pcs.h>

#define XPCS_INDIRECT_ADDR		0x8000
#define XPCS_INDIRECT_AHB_ADDR		0x83fc
#define XPCS_INDIRECT_ADDR_H		GENMASK(20, 8)
#define XPCS_INDIRECT_ADDR_L		GENMASK(7, 0)
#define XPCS_INDIRECT_DATA_ADDR(reg)	(FIELD_PREP(GENMASK(15, 10), 0x20) | \
					 FIELD_PREP(GENMASK(9, 2), \
					 FIELD_GET(XPCS_INDIRECT_ADDR_L, reg)))

/* PCS private data */
struct ipq_pcs {
	struct device *dev;
	void __iomem *base;
	struct regmap *regmap;
	phy_interface_t interface;

	/* RX clock supplied to NSSCC */
	struct clk_hw rx_hw;
	/* TX clock supplied to NSSCC */
	struct clk_hw tx_hw;
};

static unsigned long ipq_pcs_clk_rate_get(struct ipq_pcs *qpcs)
{
	switch (qpcs->interface) {
	case PHY_INTERFACE_MODE_USXGMII:
		return 312500000;
	default:
		return 125000000;
	}
}

/* Return clock rate for the RX clock supplied to NSSCC
 * as per the interface mode.
 */
static unsigned long ipq_pcs_rx_clk_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct ipq_pcs *qpcs = container_of(hw, struct ipq_pcs, rx_hw);

	return ipq_pcs_clk_rate_get(qpcs);
}

/* Return clock rate for the TX clock supplied to NSSCC
 * as per the interface mode.
 */
static unsigned long ipq_pcs_tx_clk_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct ipq_pcs *qpcs = container_of(hw, struct ipq_pcs, tx_hw);

	return ipq_pcs_clk_rate_get(qpcs);
}

static int ipq_pcs_clk_determine_rate(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	switch (req->rate) {
	case 125000000:
	case 312500000:
		return 0;
	default:
		return -EINVAL;
	}
}

/* Clock ops for the RX clock supplied to NSSCC */
static const struct clk_ops ipq_pcs_rx_clk_ops = {
	.determine_rate = ipq_pcs_clk_determine_rate,
	.recalc_rate = ipq_pcs_rx_clk_recalc_rate,
};

/* Clock ops for the TX clock supplied to NSSCC */
static const struct clk_ops ipq_pcs_tx_clk_ops = {
	.determine_rate = ipq_pcs_clk_determine_rate,
	.recalc_rate = ipq_pcs_tx_clk_recalc_rate,
};

static struct clk_hw *ipq_pcs_clk_hw_get(struct of_phandle_args *clkspec,
					 void *data)
{
	struct ipq_pcs *qpcs = data;

	switch (clkspec->args[0]) {
	case PCS_RX_CLK:
		return &qpcs->rx_hw;
	case PCS_TX_CLK:
		return &qpcs->tx_hw;
	}

	return ERR_PTR(-EINVAL);
}

/* Register the RX and TX clock which are output from SerDes to
 * the NSSCC. The NSSCC driver assigns the RX and TX clock as
 * parent, divides them to generate the MII RX and TX clock to
 * each MII interface of the PCS as per the link speeds and
 * interface modes.
 */
static int ipq_pcs_clk_register(struct ipq_pcs *qpcs)
{
	struct clk_init_data init = { };
	int ret;

	init.ops = &ipq_pcs_rx_clk_ops;
	init.name = devm_kasprintf(qpcs->dev, GFP_KERNEL, "%s::rx_clk",
				   dev_name(qpcs->dev));
	if (!init.name)
		return -ENOMEM;

	qpcs->rx_hw.init = &init;
	ret = devm_clk_hw_register(qpcs->dev, &qpcs->rx_hw);
	if (ret)
		return ret;

	init.ops = &ipq_pcs_tx_clk_ops;
	init.name = devm_kasprintf(qpcs->dev, GFP_KERNEL, "%s::tx_clk",
				   dev_name(qpcs->dev));
	if (!init.name)
		return -ENOMEM;

	qpcs->tx_hw.init = &init;
	ret = devm_clk_hw_register(qpcs->dev, &qpcs->tx_hw);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(qpcs->dev, ipq_pcs_clk_hw_get, qpcs);
}

static int ipq_pcs_regmap_read(void *context, unsigned int reg,
			       unsigned int *val)
{
	struct ipq_pcs *qpcs = context;

	/* PCS uses direct AHB access while XPCS uses indirect AHB access */
	if (reg >= XPCS_INDIRECT_ADDR) {
		writel(FIELD_GET(XPCS_INDIRECT_ADDR_H, reg),
		       qpcs->base + XPCS_INDIRECT_AHB_ADDR);
		*val = readl(qpcs->base + XPCS_INDIRECT_DATA_ADDR(reg));
	} else {
		*val = readl(qpcs->base + reg);
	}

	return 0;
}

static int ipq_pcs_regmap_write(void *context, unsigned int reg,
				unsigned int val)
{
	struct ipq_pcs *qpcs = context;

	/* PCS uses direct AHB access while XPCS uses indirect AHB access */
	if (reg >= XPCS_INDIRECT_ADDR) {
		writel(FIELD_GET(XPCS_INDIRECT_ADDR_H, reg),
		       qpcs->base + XPCS_INDIRECT_AHB_ADDR);
		writel(val, qpcs->base + XPCS_INDIRECT_DATA_ADDR(reg));
	} else {
		writel(val, qpcs->base + reg);
	}

	return 0;
}

static const struct regmap_config ipq_pcs_regmap_cfg = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_read = ipq_pcs_regmap_read,
	.reg_write = ipq_pcs_regmap_write,
	.fast_io = true,
};

static int ipq9574_pcs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ipq_pcs *qpcs;
	struct clk *clk;
	int ret;

	qpcs = devm_kzalloc(dev, sizeof(*qpcs), GFP_KERNEL);
	if (!qpcs)
		return -ENOMEM;

	qpcs->dev = dev;

	qpcs->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(qpcs->base))
		return dev_err_probe(dev, PTR_ERR(qpcs->base),
				     "Failed to ioremap resource\n");

	qpcs->regmap = devm_regmap_init(dev, NULL, qpcs, &ipq_pcs_regmap_cfg);
	if (IS_ERR(qpcs->regmap))
		return dev_err_probe(dev, PTR_ERR(qpcs->regmap),
				     "Failed to allocate register map\n");

	clk = devm_clk_get_enabled(dev, "sys");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Failed to enable SYS clock\n");

	clk = devm_clk_get_enabled(dev, "ahb");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Failed to enable AHB clock\n");

	ret = ipq_pcs_clk_register(qpcs);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, qpcs);

	return 0;
}

static const struct of_device_id ipq9574_pcs_of_mtable[] = {
	{ .compatible = "qcom,ipq9574-pcs" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ipq9574_pcs_of_mtable);

static struct platform_driver ipq9574_pcs_driver = {
	.driver = {
		.name = "ipq9574_pcs",
		.suppress_bind_attrs = true,
		.of_match_table = ipq9574_pcs_of_mtable,
	},
	.probe = ipq9574_pcs_probe,
};
module_platform_driver(ipq9574_pcs_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm IPQ9574 PCS driver");
MODULE_AUTHOR("Lei Wei <quic_leiwei@quicinc.com>");
