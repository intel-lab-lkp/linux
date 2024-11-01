// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pcs/pcs-qcom-ipq.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/net/pcs-qcom-ipq.h>

/* Maximum number of MIIs per PCS instance. There are 5 MIIs for PSGMII. */
#define PCS_MAX_MII_NRS			5

#define PCS_CALIBRATION			0x1e0
#define PCS_CALIBRATION_DONE		BIT(7)

#define PCS_MODE_CTRL			0x46c
#define PCS_MODE_SEL_MASK		GENMASK(12, 8)
#define PCS_MODE_SGMII			FIELD_PREP(PCS_MODE_SEL_MASK, 0x4)
#define PCS_MODE_QSGMII			FIELD_PREP(PCS_MODE_SEL_MASK, 0x1)
#define PCS_MODE_AN_MODE		BIT(0)

#define PCS_MII_CTRL(x)			(0x480 + 0x18 * (x))
#define PCS_MII_ADPT_RESET		BIT(11)
#define PCS_MII_FORCE_MODE		BIT(3)
#define PCS_MII_SPEED_MASK		GENMASK(2, 1)
#define PCS_MII_SPEED_1000		FIELD_PREP(PCS_MII_SPEED_MASK, 0x2)
#define PCS_MII_SPEED_100		FIELD_PREP(PCS_MII_SPEED_MASK, 0x1)
#define PCS_MII_SPEED_10		FIELD_PREP(PCS_MII_SPEED_MASK, 0x0)

#define PCS_MII_STS(x)			(0x488 + 0x18 * (x))
#define PCS_MII_LINK_STS		BIT(7)
#define PCS_MII_STS_DUPLEX_FULL		BIT(6)
#define PCS_MII_STS_SPEED_MASK		GENMASK(5, 4)
#define PCS_MII_STS_SPEED_10		0
#define PCS_MII_STS_SPEED_100		1
#define PCS_MII_STS_SPEED_1000		2
#define PCS_MII_STS_PAUSE_TX_EN		BIT(1)
#define PCS_MII_STS_PAUSE_RX_EN		BIT(0)

#define PCS_PLL_RESET			0x780
#define PCS_ANA_SW_RESET		BIT(6)

#define XPCS_INDIRECT_ADDR		0x8000
#define XPCS_INDIRECT_AHB_ADDR		0x83fc
#define XPCS_INDIRECT_ADDR_H		GENMASK(20, 8)
#define XPCS_INDIRECT_ADDR_L		GENMASK(7, 0)
#define XPCS_INDIRECT_DATA_ADDR(reg)	(FIELD_PREP(GENMASK(15, 10), 0x20) | \
					 FIELD_PREP(GENMASK(9, 2), \
					 FIELD_GET(XPCS_INDIRECT_ADDR_L, reg)))

/* Private data for the PCS instance */
struct ipq_pcs {
	struct device *dev;
	void __iomem *base;
	struct regmap *regmap;
	phy_interface_t interface;

	/* Lock to protect PCS configurations shared by multiple MII ports */
	struct mutex config_lock;

	/* RX clock supplied to NSSCC */
	struct clk_hw rx_hw;
	/* TX clock supplied to NSSCC */
	struct clk_hw tx_hw;
};

/* PCS MII clock ID */
enum {
	PCS_MII_RX_CLK,
	PCS_MII_TX_CLK,
	PCS_MII_CLK_MAX
};

/* PCS MII clock name */
static const char *const pcs_mii_clk_name[PCS_MII_CLK_MAX] = {
	"mii_rx",
	"mii_tx",
};

/* Per PCS MII private data */
struct ipq_pcs_mii {
	struct ipq_pcs *qpcs;
	struct phylink_pcs pcs;
	int index;

	/* Rx/Tx clocks from NSSCC to PCS MII */
	struct clk *clk[PCS_MII_CLK_MAX];
};

#define phylink_pcs_to_qpcs_mii(_pcs)	\
	container_of(_pcs, struct ipq_pcs_mii, pcs)

static void ipq_pcs_get_state_sgmii(struct ipq_pcs *qpcs,
				    int index,
				    struct phylink_link_state *state)
{
	unsigned int val;
	int ret;

	ret = regmap_read(qpcs->regmap, PCS_MII_STS(index), &val);
	if (ret) {
		state->link = 0;
		return;
	}

	state->link = !!(val & PCS_MII_LINK_STS);

	if (!state->link)
		return;

	switch (FIELD_GET(PCS_MII_STS_SPEED_MASK, val)) {
	case PCS_MII_STS_SPEED_1000:
		state->speed = SPEED_1000;
		break;
	case PCS_MII_STS_SPEED_100:
		state->speed = SPEED_100;
		break;
	case PCS_MII_STS_SPEED_10:
		state->speed = SPEED_10;
		break;
	default:
		state->link = false;
		return;
	}

	if (val & PCS_MII_STS_DUPLEX_FULL)
		state->duplex = DUPLEX_FULL;
	else
		state->duplex = DUPLEX_HALF;

	if (val & PCS_MII_STS_PAUSE_TX_EN)
		state->pause |= MLO_PAUSE_TX;

	if (val & PCS_MII_STS_PAUSE_RX_EN)
		state->pause |= MLO_PAUSE_RX;
}

static int ipq_pcs_config_mode(struct ipq_pcs *qpcs,
			       phy_interface_t interface)
{
	unsigned int val;
	int ret;

	/* Configure PCS interface mode */
	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
		/* Select Qualcomm SGMII AN mode */
		ret = regmap_update_bits(qpcs->regmap, PCS_MODE_CTRL,
					 PCS_MODE_SEL_MASK | PCS_MODE_AN_MODE,
					 PCS_MODE_SGMII);
		if (ret)
			return ret;
		break;
	case PHY_INTERFACE_MODE_QSGMII:
		ret = regmap_update_bits(qpcs->regmap, PCS_MODE_CTRL,
					 PCS_MODE_SEL_MASK | PCS_MODE_AN_MODE,
					 PCS_MODE_QSGMII);
		if (ret)
			return ret;
		break;
	default:
		dev_err(qpcs->dev,
			"Unsupported interface %s\n", phy_modes(interface));
		return -EOPNOTSUPP;
	}

	/* PCS PLL reset */
	ret = regmap_update_bits(qpcs->regmap, PCS_PLL_RESET,
				 PCS_ANA_SW_RESET, 0);
	if (ret)
		return ret;

	fsleep(1000);
	ret = regmap_update_bits(qpcs->regmap, PCS_PLL_RESET,
				 PCS_ANA_SW_RESET, PCS_ANA_SW_RESET);
	if (ret)
		return ret;

	/* Wait for calibration completion */
	ret = regmap_read_poll_timeout(qpcs->regmap, PCS_CALIBRATION,
				       val, val & PCS_CALIBRATION_DONE,
				       1000, 100000);
	if (ret) {
		dev_err(qpcs->dev, "PCS calibration timed-out\n");
		return ret;
	}

	qpcs->interface = interface;

	return 0;
}

static int ipq_pcs_config_sgmii(struct ipq_pcs *qpcs,
				int index,
				unsigned int neg_mode,
				phy_interface_t interface)
{
	int ret;

	/* Access to PCS registers such as PCS_MODE_CTRL which are
	 * common to all MIIs, is lock protected and configured
	 * only once. This is required only for interface modes
	 * such as QSGMII.
	 */
	if (interface == PHY_INTERFACE_MODE_QSGMII)
		mutex_lock(&qpcs->config_lock);

	if (qpcs->interface != interface) {
		ret = ipq_pcs_config_mode(qpcs, interface);
		if (ret)
			goto err;
	}

	if (interface == PHY_INTERFACE_MODE_QSGMII)
		mutex_unlock(&qpcs->config_lock);

	/* Nothing to do here as in-band autoneg mode is enabled
	 * by default for each PCS MII port.
	 */
	if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED)
		return 0;

	/* Set force speed mode */
	return regmap_update_bits(qpcs->regmap, PCS_MII_CTRL(index),
				  PCS_MII_FORCE_MODE, PCS_MII_FORCE_MODE);

err:
	if (interface == PHY_INTERFACE_MODE_QSGMII)
		mutex_unlock(&qpcs->config_lock);

	return ret;
}

static int ipq_pcs_link_up_config_sgmii(struct ipq_pcs *qpcs,
					int index,
					unsigned int neg_mode,
					int speed)
{
	int ret;

	/* PCS speed need not be configured if in-band autoneg is enabled */
	if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED)
		goto pcs_adapter_reset;

	/* PCS speed set for force mode */
	switch (speed) {
	case SPEED_1000:
		ret = regmap_update_bits(qpcs->regmap, PCS_MII_CTRL(index),
					 PCS_MII_SPEED_MASK,
					 PCS_MII_SPEED_1000);
		if (ret)
			return ret;
		break;
	case SPEED_100:
		ret = regmap_update_bits(qpcs->regmap, PCS_MII_CTRL(index),
					 PCS_MII_SPEED_MASK, PCS_MII_SPEED_100);
		if (ret)
			return ret;
		break;
	case SPEED_10:
		ret = regmap_update_bits(qpcs->regmap, PCS_MII_CTRL(index),
					 PCS_MII_SPEED_MASK, PCS_MII_SPEED_10);
		if (ret)
			return ret;
		break;
	default:
		dev_err(qpcs->dev, "Invalid SGMII speed %d\n", speed);
		return -EINVAL;
	}

pcs_adapter_reset:
	/* PCS adapter reset */
	ret = regmap_update_bits(qpcs->regmap, PCS_MII_CTRL(index),
				 PCS_MII_ADPT_RESET, 0);
	if (ret)
		return ret;

	return regmap_update_bits(qpcs->regmap, PCS_MII_CTRL(index),
				  PCS_MII_ADPT_RESET, PCS_MII_ADPT_RESET);
}

static void ipq_pcs_get_state(struct phylink_pcs *pcs,
			      struct phylink_link_state *state)
{
	struct ipq_pcs_mii *qpcs_mii = phylink_pcs_to_qpcs_mii(pcs);
	struct ipq_pcs *qpcs = qpcs_mii->qpcs;
	int index = qpcs_mii->index;

	switch (state->interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
		ipq_pcs_get_state_sgmii(qpcs, index, state);
		break;
	default:
		break;
	}

	dev_dbg(qpcs->dev,
		"mode=%s/%s/%s link=%u\n",
		phy_modes(state->interface),
		phy_speed_to_str(state->speed),
		phy_duplex_to_str(state->duplex),
		state->link);
}

static int ipq_pcs_config(struct phylink_pcs *pcs,
			  unsigned int neg_mode,
			  phy_interface_t interface,
			  const unsigned long *advertising,
			  bool permit)
{
	struct ipq_pcs_mii *qpcs_mii = phylink_pcs_to_qpcs_mii(pcs);
	struct ipq_pcs *qpcs = qpcs_mii->qpcs;
	int index = qpcs_mii->index;

	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
		return ipq_pcs_config_sgmii(qpcs, index, neg_mode, interface);
	default:
		dev_err(qpcs->dev,
			"Unsupported interface %s\n", phy_modes(interface));
		return -EOPNOTSUPP;
	};
}

static void ipq_pcs_link_up(struct phylink_pcs *pcs,
			    unsigned int neg_mode,
			    phy_interface_t interface,
			    int speed, int duplex)
{
	struct ipq_pcs_mii *qpcs_mii = phylink_pcs_to_qpcs_mii(pcs);
	struct ipq_pcs *qpcs = qpcs_mii->qpcs;
	int index = qpcs_mii->index;
	int ret;

	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_QSGMII:
		ret = ipq_pcs_link_up_config_sgmii(qpcs, index,
						   neg_mode, speed);
		break;
	default:
		dev_err(qpcs->dev,
			"Unsupported interface %s\n", phy_modes(interface));
		return;
	}

	if (ret)
		dev_err(qpcs->dev, "PCS link up fail for interface %s\n",
			phy_modes(interface));
}

static const struct phylink_pcs_ops ipq_pcs_phylink_ops = {
	.pcs_get_state = ipq_pcs_get_state,
	.pcs_config = ipq_pcs_config,
	.pcs_link_up = ipq_pcs_link_up,
};

/**
 * ipq_pcs_create() - Create an IPQ PCS MII instance
 * @np: Device tree node to the PCS MII
 *
 * Description: Create a phylink PCS instance for the given PCS MII node @np
 * and enable the MII clocks. This instance is associated with the specific
 * MII of the PCS and the corresponding Ethernet netdevice.
 *
 * Return: A pointer to the phylink PCS instance or an error-pointer value.
 */
struct phylink_pcs *ipq_pcs_create(struct device_node *np)
{
	struct platform_device *pdev;
	struct ipq_pcs_mii *qpcs_mii;
	struct device_node *pcs_np;
	struct ipq_pcs *qpcs;
	int i, ret;
	u32 index;

	if (!of_device_is_available(np))
		return ERR_PTR(-ENODEV);

	if (of_property_read_u32(np, "reg", &index))
		return ERR_PTR(-EINVAL);

	if (index >= PCS_MAX_MII_NRS)
		return ERR_PTR(-EINVAL);

	pcs_np = of_get_parent(np);
	if (!pcs_np)
		return ERR_PTR(-ENODEV);

	if (!of_device_is_available(pcs_np)) {
		of_node_put(pcs_np);
		return ERR_PTR(-ENODEV);
	}

	pdev = of_find_device_by_node(pcs_np);
	of_node_put(pcs_np);
	if (!pdev)
		return ERR_PTR(-ENODEV);

	qpcs = platform_get_drvdata(pdev);
	put_device(&pdev->dev);

	/* If probe is not yet completed, return DEFER to
	 * the dependent driver.
	 */
	if (!qpcs)
		return ERR_PTR(-EPROBE_DEFER);

	qpcs_mii = kzalloc(sizeof(*qpcs_mii), GFP_KERNEL);
	if (!qpcs_mii)
		return ERR_PTR(-ENOMEM);

	qpcs_mii->qpcs = qpcs;
	qpcs_mii->index = index;
	qpcs_mii->pcs.ops = &ipq_pcs_phylink_ops;
	qpcs_mii->pcs.neg_mode = true;
	qpcs_mii->pcs.poll = true;

	for (i = 0; i < PCS_MII_CLK_MAX; i++) {
		qpcs_mii->clk[i] = of_clk_get_by_name(np, pcs_mii_clk_name[i]);
		if (IS_ERR(qpcs_mii->clk[i])) {
			dev_err(qpcs->dev,
				"Failed to get MII %d interface clock %s\n",
				index, pcs_mii_clk_name[i]);
			goto err_clk_get;
		}

		ret = clk_prepare_enable(qpcs_mii->clk[i]);
		if (ret) {
			dev_err(qpcs->dev,
				"Failed to enable MII %d interface clock %s\n",
				index, pcs_mii_clk_name[i]);
			goto err_clk_en;
		}
	}

	return &qpcs_mii->pcs;

err_clk_en:
	clk_put(qpcs_mii->clk[i]);
err_clk_get:
	while (i) {
		i--;
		clk_disable_unprepare(qpcs_mii->clk[i]);
		clk_put(qpcs_mii->clk[i]);
	}

	kfree(qpcs_mii);
	return ERR_PTR(-ENODEV);
}
EXPORT_SYMBOL(ipq_pcs_create);

/**
 * ipq_pcs_destroy() - Destroy the IPQ PCS MII instance
 * @pcs: PCS instance
 *
 * Description: Destroy a phylink PCS instance.
 */
void ipq_pcs_destroy(struct phylink_pcs *pcs)
{
	struct ipq_pcs_mii *qpcs_mii;
	int i;

	if (!pcs)
		return;

	qpcs_mii = phylink_pcs_to_qpcs_mii(pcs);

	for (i = 0; i < PCS_MII_CLK_MAX; i++) {
		clk_disable_unprepare(qpcs_mii->clk[i]);
		clk_put(qpcs_mii->clk[i]);
	}

	kfree(qpcs_mii);
}
EXPORT_SYMBOL(ipq_pcs_destroy);

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
static const struct clk_ops qpcs_rx_clk_ops = {
	.determine_rate = ipq_pcs_clk_determine_rate,
	.recalc_rate = ipq_pcs_rx_clk_recalc_rate,
};

/* Clock ops for the TX clock supplied to NSSCC */
static const struct clk_ops qpcs_tx_clk_ops = {
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

	init.ops = &qpcs_rx_clk_ops;
	init.name = devm_kasprintf(qpcs->dev, GFP_KERNEL, "%s::rx_clk",
				   dev_name(qpcs->dev));
	if (!init.name)
		return -ENOMEM;

	qpcs->rx_hw.init = &init;
	ret = devm_clk_hw_register(qpcs->dev, &qpcs->rx_hw);
	if (ret)
		return ret;

	init.ops = &qpcs_tx_clk_ops;
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

static int ipq_pcs_probe(struct platform_device *pdev)
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

	mutex_init(&qpcs->config_lock);

	platform_set_drvdata(pdev, qpcs);

	return 0;
}

static const struct of_device_id ipq_pcs_of_mtable[] = {
	{ .compatible = "qcom,ipq9574-pcs" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ipq_pcs_of_mtable);

static struct platform_driver ipq_pcs_driver = {
	.driver = {
		.name = "ipq_pcs",
		.of_match_table = ipq_pcs_of_mtable,
	},
	.probe = ipq_pcs_probe,
};
module_platform_driver(ipq_pcs_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm IPQ PCS driver");
MODULE_AUTHOR("Lei Wei <quic_leiwei@quicinc.com>");
