// SPDX-License-Identifier: GPL-2.0
/*
 * NXP S32G/R GMAC glue layer
 *
 * Copyright 2019-2026 NXP
 *
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_mdio.h>
#include <linux/of_address.h>
#include <linux/phy.h>
#include <linux/phy/nxp-s32g-serdes.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/stmmac.h>

#include "stmmac_platform.h"
#include "stmmac.h"

#define GMAC_INTF_RATE_125M	125000000	/* 125MHz */

/* SoC PHY interface control register */
#define S32_PHY_INTF_SEL_MII	0x00
#define S32_PHY_INTF_SEL_SGMII	0x01
#define S32_PHY_INTF_SEL_RGMII	0x02
#define S32_PHY_INTF_SEL_RMII	0x08

struct s32_priv_data {
	void __iomem *ioaddr;
	void __iomem *ctrl_sts;
	struct regmap *sts_regmap;
	unsigned int sts_offset;
	struct device *dev;
	phy_interface_t *intf_mode;
	struct clk *tx_clk;
	struct clk *rx_clk;
	/* SGMII PCS provided by the SerDes subsystem (NULL if not SGMII) */
	struct phylink_pcs *pcs;
};

static int s32_gmac_write_phy_intf_select(struct s32_priv_data *gmac)
{
	u32 intf_sel;
	int ret = 0;

	switch (*gmac->intf_mode) {
	case PHY_INTERFACE_MODE_SGMII:
		intf_sel = S32_PHY_INTF_SEL_SGMII;
		break;
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		intf_sel = S32_PHY_INTF_SEL_RGMII;
		break;
	case PHY_INTERFACE_MODE_RMII:
		intf_sel = S32_PHY_INTF_SEL_RMII;
		break;
	case PHY_INTERFACE_MODE_MII:
		intf_sel = S32_PHY_INTF_SEL_MII;
		break;
	default:
		dev_err(gmac->dev, "Unsupported phy interface mode: %s\n",
			phy_modes(*gmac->intf_mode));
		return -EINVAL;
	}

	if (gmac->ctrl_sts)
		writel(intf_sel, gmac->ctrl_sts);
	else
		ret = regmap_write(gmac->sts_regmap, gmac->sts_offset,
				   intf_sel);

	dev_dbg(gmac->dev, "PHY mode set to %s\n", phy_modes(*gmac->intf_mode));

	return ret;
}

static int s32_gmac_pcs_init(struct stmmac_priv *priv)
{
	struct s32_priv_data *gmac = priv->plat->bsp_priv;
	struct device_node *pcs_node;

	/*
	 * pcs-handle points at the SerDes XPCS lane child node; its
	 * nxp,xpcs-instance property is what identifies the XPCS instance.
	 */
	pcs_node = of_parse_phandle(gmac->dev->of_node, "pcs-handle", 0);
	if (!pcs_node) {
		dev_dbg(gmac->dev, "no 'pcs-handle' property, SGMII PCS unavailable\n");
		return 0;
	}

	gmac->pcs = s32g_serdes_pcs_create(gmac->dev, pcs_node);
	of_node_put(pcs_node);

	if (IS_ERR(gmac->pcs)) {
		int err = PTR_ERR(gmac->pcs);

		gmac->pcs = NULL;
		/* SerDes may not be probed yet - defer probe. */
		return err;
	}

	dev_dbg(gmac->dev, "SGMII PCS created via SerDes XPCS\n");
	return 0;
}

static struct phylink_pcs *s32_gmac_select_pcs(struct stmmac_priv *priv,
					       phy_interface_t interface)
{
	struct s32_priv_data *gmac = priv->plat->bsp_priv;

	if (interface == PHY_INTERFACE_MODE_SGMII && gmac->pcs)
		return gmac->pcs;

	return NULL;
}

static int s32_gmac_init(struct device *dev, void *priv)
{
	struct s32_priv_data *gmac = priv;
	int ret;

	/* Set initial TX interface clock */
	ret = clk_prepare_enable(gmac->tx_clk);
	if (ret) {
		dev_err(dev, "Can't enable tx clock\n");
		return ret;
	}
	ret = clk_set_rate(gmac->tx_clk, GMAC_INTF_RATE_125M);
	if (ret) {
		dev_err(dev, "Can't set tx clock\n");
		goto err_tx_disable;
	}

	/* Set initial RX interface clock */
	ret = clk_prepare_enable(gmac->rx_clk);
	if (ret) {
		dev_err(dev, "Can't enable rx clock\n");
		goto err_tx_disable;
	}
	ret = clk_set_rate(gmac->rx_clk, GMAC_INTF_RATE_125M);
	if (ret) {
		dev_err(dev, "Can't set rx clock\n");
		goto err_txrx_disable;
	}

	/* Set interface mode */
	ret = s32_gmac_write_phy_intf_select(gmac);
	if (ret) {
		dev_err(dev, "Can't set PHY interface mode\n");
		goto err_txrx_disable;
	}

	return 0;

err_txrx_disable:
	clk_disable_unprepare(gmac->rx_clk);
err_tx_disable:
	clk_disable_unprepare(gmac->tx_clk);
	return ret;
}

static void s32_gmac_exit(struct device *dev, void *priv)
{
	struct s32_priv_data *gmac = priv;

	clk_disable_unprepare(gmac->tx_clk);
	clk_disable_unprepare(gmac->rx_clk);
}

static void s32_gmac_setup_multi_irq(struct device *dev,
				     struct plat_stmmacenet_data *plat,
				     struct stmmac_resources *res)
{
	int i;

	/* RX IRQs */
	for (i = 0; i < plat->rx_queues_to_use; i++) {
		if (res->rx_irq[i] <= 0) {
			dev_dbg(dev, "Missing RX queue %d interrupt\n", i);
			goto mac_irq_mode;
		}
	}

	/* TX IRQs */
	for (i = 0; i < plat->tx_queues_to_use; i++) {
		if (res->tx_irq[i] <= 0) {
			dev_dbg(dev, "Missing TX queue %d interrupt\n", i);
			goto mac_irq_mode;
		}
	}

	plat->flags |= STMMAC_FLAG_MULTI_MSI_EN;
	dev_info(dev, "Multi-IRQ mode (per queue IRQs) selected\n");
	return;

mac_irq_mode:
	plat->flags &= ~STMMAC_FLAG_MULTI_MSI_EN;
	dev_info(dev, "MAC IRQ mode selected\n");
}

static int s32_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat;
	struct device *dev = &pdev->dev;
	struct stmmac_resources res;
	struct s32_priv_data *gmac;
	int ret;

	gmac = devm_kzalloc(&pdev->dev, sizeof(*gmac), GFP_KERNEL);
	if (!gmac)
		return -ENOMEM;

	gmac->dev = &pdev->dev;

	ret = stmmac_get_platform_resources(pdev, &res);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to get platform resources\n");

	plat = devm_stmmac_probe_config_dt(pdev, res.mac);
	if (IS_ERR(plat))
		return dev_err_probe(dev, PTR_ERR(plat),
				     "dt configuration failed\n");

	/* PHY interface mode control reg */
	gmac->sts_regmap = syscon_regmap_lookup_by_phandle_args(dev->of_node,
					"nxp,phy-sel", 1, &gmac->sts_offset);
	if (gmac->sts_regmap == ERR_PTR(-EPROBE_DEFER))
		return PTR_ERR(gmac->sts_regmap);
	if (IS_ERR(gmac->sts_regmap)) {
		gmac->ctrl_sts = devm_platform_get_and_ioremap_resource(pdev, 1, NULL);
		if (IS_ERR(gmac->ctrl_sts))
			return dev_err_probe(dev, PTR_ERR(gmac->ctrl_sts),
					     "S32CC config region is missing\n");
	}

	/* tx clock */
	gmac->tx_clk = devm_clk_get(&pdev->dev, "tx");
	if (IS_ERR(gmac->tx_clk))
		return dev_err_probe(dev, PTR_ERR(gmac->tx_clk),
				     "tx clock not found\n");

	/* rx clock */
	gmac->rx_clk = devm_clk_get(&pdev->dev, "rx");
	if (IS_ERR(gmac->rx_clk))
		return dev_err_probe(dev, PTR_ERR(gmac->rx_clk),
				     "rx clock not found\n");

	gmac->intf_mode = &plat->phy_interface;
	gmac->ioaddr = res.addr;

	/* S32CC core feature set */
	plat->core_type = DWMAC_CORE_GMAC4;
	plat->pmt = true;
	plat->flags |= STMMAC_FLAG_SPH_DISABLE;

	s32_gmac_setup_multi_irq(dev, plat, &res);

	plat->rx_fifo_size = 20480;
	plat->tx_fifo_size = 20480;

	plat->init = s32_gmac_init;
	plat->exit = s32_gmac_exit;
	plat->pcs_init = s32_gmac_pcs_init;
	plat->select_pcs = s32_gmac_select_pcs;

	plat->clk_tx_i = gmac->tx_clk;
	plat->set_clk_tx_rate = stmmac_set_clk_tx_rate;

	plat->bsp_priv = gmac;

	return stmmac_pltfr_probe(pdev, plat, &res);
}

static const struct of_device_id s32_dwmac_match[] = {
	{ .compatible = "nxp,s32g2-dwmac" },
	{ }
};
MODULE_DEVICE_TABLE(of, s32_dwmac_match);

static struct platform_driver s32_dwmac_driver = {
	.probe = s32_dwmac_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name = "s32-dwmac",
		.pm = &stmmac_pltfr_pm_ops,
		.of_match_table = s32_dwmac_match,
	},
};
module_platform_driver(s32_dwmac_driver);

MODULE_AUTHOR("Jan Petrous (OSS) <jan.petrous@oss.nxp.com>");
MODULE_DESCRIPTION("NXP S32G/R common chassis GMAC driver");
MODULE_LICENSE("GPL");

