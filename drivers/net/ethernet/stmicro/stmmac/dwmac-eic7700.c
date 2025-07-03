// SPDX-License-Identifier: GPL-2.0
/*
 * Eswin DWC Ethernet linux driver
 *
 * Copyright 2025, Beijing ESWIN Computing Technology Co., Ltd.
 *
 * Authors:
 *   Shuang Liang <liangshuang@eswincomputing.com>
 *   Shangjuan Wei <weishangjuan@eswincomputing.com>
 */

#include <linux/platform_device.h>
#include <linux/mfd/syscon.h>
#include <linux/stmmac.h>
#include <linux/regmap.h>
#include <linux/of.h>

#include "stmmac_platform.h"

/* eth_phy_ctrl_offset eth0:0x100; eth1:0x200 */
#define EIC7700_ETH_TX_CLK_SEL		BIT(16)
#define EIC7700_ETH_PHY_INTF_SELI	BIT(0)

/* eth_axi_lp_ctrl_offset eth0:0x108; eth1:0x208 */
#define EIC7700_ETH_CSYSREQ_VAL		BIT(0)

/* hsp_aclk_ctrl_offset (0x148) */
#define EIC7700_HSP_ACLK_CLKEN		BIT(31)
#define EIC7700_HSP_ACLK_DIVSOR		(0x2 << 4)

/* hsp_cfg_ctrl_offset (0x14c) */
#define EIC7700_HSP_CFG_CLKEN		BIT(31)
#define EIC7700_SCU_HSP_PCLK_EN		BIT(30)
#define EIC7700_HSP_CFG_CTRL_REGSET	(EIC7700_HSP_CFG_CLKEN | EIC7700_SCU_HSP_PCLK_EN)

/* TX/RX clock delay (unit: 0.1ns per bit) */
#define EIC7700_ETH_TX_ADJ_DELAY	GENMASK(14, 8)
#define EIC7700_ETH_RX_ADJ_DELAY	GENMASK(30, 24)

/* Default delay value*/
#define EIC7700_DELAY_VALUE0 0x20202020
#define EIC7700_DELAY_VALUE1 0x96205A20

struct eic7700_qos_priv {
	struct device *dev;
	struct regmap *crg_regmap;
	struct regmap *hsp_regmap;
	u32 tx_delay_ps;
	u32 rx_delay_ps;
	u32 dly_hsp_reg[3];
	u32 dly_param_1000m[3];
	u32 dly_param_100m[3];
	u32 dly_param_10m[3];
};

static inline void eic7700_set_delay(u32 rx_ps, u32 tx_ps, u32 *reg)
{
	u32 rx_val = rx_ps / 100;

	if (rx_val > 0x7F)
		rx_val = 0x7F;

	*reg &= ~EIC7700_ETH_RX_ADJ_DELAY;
	*reg |= (rx_val << 24) & EIC7700_ETH_RX_ADJ_DELAY;

	u32 tx_val = tx_ps / 100;

	if (tx_val > 0x7F)
		tx_val = 0x7F;

	*reg &= ~EIC7700_ETH_TX_ADJ_DELAY;
	*reg |= (tx_val << 8) & EIC7700_ETH_TX_ADJ_DELAY;
}

static void eic7700_qos_fix_speed(void *priv, int speed, u32 mode)
{
	struct eic7700_qos_priv *dwc_priv = priv;
	int i;

	switch (speed) {
	case SPEED_1000:
		for (i = 0; i < 3; i++)
			regmap_write(dwc_priv->hsp_regmap,
				     dwc_priv->dly_hsp_reg[i],
				     dwc_priv->dly_param_1000m[i]);
		break;
	case SPEED_100:
		for (i = 0; i < 3; i++) {
			regmap_write(dwc_priv->hsp_regmap,
				     dwc_priv->dly_hsp_reg[i],
				     dwc_priv->dly_param_100m[i]);
		}
		break;
	case SPEED_10:
		for (i = 0; i < 3; i++) {
			regmap_write(dwc_priv->hsp_regmap,
				     dwc_priv->dly_hsp_reg[i],
				     dwc_priv->dly_param_10m[i]);
		}
		break;
	default:
		dev_err(dwc_priv->dev, "invalid speed %u\n", speed);
		break;
	}
}

static int eic7700_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct eic7700_qos_priv *dwc_priv;
	u32 hsp_aclk_ctrl_offset;
	u32 hsp_aclk_ctrl_regset;
	u32 hsp_cfg_ctrl_offset;
	u32 eth_axi_lp_ctrl_offset;
	u32 eth_phy_ctrl_offset;
	u32 eth_phy_ctrl_regset;
	bool has_rx_dly = false;
	bool has_tx_dly = false;
	int ret;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				"failed to get resources\n");

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return dev_err_probe(&pdev->dev, PTR_ERR(plat_dat),
				"dt configuration failed\n");

	dwc_priv = devm_kzalloc(&pdev->dev, sizeof(*dwc_priv), GFP_KERNEL);
	if (!dwc_priv)
		return -ENOMEM;

	dwc_priv->dev = &pdev->dev;
	dwc_priv->dly_param_1000m[0] = EIC7700_DELAY_VALUE0;
	dwc_priv->dly_param_1000m[1] = EIC7700_DELAY_VALUE1;
	dwc_priv->dly_param_1000m[2] = EIC7700_DELAY_VALUE0;
	dwc_priv->dly_param_100m[0] = EIC7700_DELAY_VALUE0;
	dwc_priv->dly_param_100m[1] = EIC7700_DELAY_VALUE1;
	dwc_priv->dly_param_100m[2] = EIC7700_DELAY_VALUE0;
	dwc_priv->dly_param_10m[0] = 0x0;
	dwc_priv->dly_param_10m[1] = 0x0;
	dwc_priv->dly_param_10m[2] = 0x0;

	ret = of_property_read_u32(pdev->dev.of_node, "rx-internal-delay-ps",
				   &dwc_priv->rx_delay_ps);
	if (ret)
		dev_dbg(&pdev->dev, "can't get rx-internal-delay-ps, ret(%d).", ret);
	else
		has_rx_dly = true;

	ret = of_property_read_u32(pdev->dev.of_node, "tx-internal-delay-ps",
				   &dwc_priv->tx_delay_ps);
	if (ret)
		dev_dbg(&pdev->dev, "can't get tx-internal-delay-ps, ret(%d).", ret);
	else
		has_tx_dly = true;
	if (has_rx_dly && has_tx_dly) {
		eic7700_set_delay(dwc_priv->rx_delay_ps, dwc_priv->tx_delay_ps,
				  &dwc_priv->dly_param_1000m[1]);
		eic7700_set_delay(dwc_priv->rx_delay_ps, dwc_priv->tx_delay_ps,
				  &dwc_priv->dly_param_100m[1]);
		eic7700_set_delay(dwc_priv->rx_delay_ps, dwc_priv->tx_delay_ps,
				  &dwc_priv->dly_param_10m[1]);
	} else {
		dev_dbg(&pdev->dev, " use default dly\n");
	}

	ret = of_property_read_variable_u32_array(pdev->dev.of_node, "eswin,dly_hsp_reg",
						  &dwc_priv->dly_hsp_reg[0], 3, 0);
	if (ret != 3) {
		dev_err(&pdev->dev, "can't get delay hsp reg.ret(%d)\n", ret);
		return ret;
	}

	dwc_priv->crg_regmap = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							       "eswin,syscrg_csr");
	if (IS_ERR(dwc_priv->crg_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(dwc_priv->crg_regmap),
				"Failed to get syscrg_csr regmap\n");

	ret = of_property_read_u32_index(pdev->dev.of_node, "eswin,syscrg_csr", 1,
					 &hsp_aclk_ctrl_offset);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "can't get hsp_aclk_ctrl_offset\n");

	regmap_read(dwc_priv->crg_regmap, hsp_aclk_ctrl_offset, &hsp_aclk_ctrl_regset);
	hsp_aclk_ctrl_regset |= (EIC7700_HSP_ACLK_CLKEN | EIC7700_HSP_ACLK_DIVSOR);
	regmap_write(dwc_priv->crg_regmap, hsp_aclk_ctrl_offset, hsp_aclk_ctrl_regset);

	ret = of_property_read_u32_index(pdev->dev.of_node, "eswin,syscrg_csr", 2,
					 &hsp_cfg_ctrl_offset);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "can't get hsp_cfg_ctrl_offset\n");

	regmap_write(dwc_priv->crg_regmap, hsp_cfg_ctrl_offset, EIC7700_HSP_CFG_CTRL_REGSET);

	dwc_priv->hsp_regmap = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
							       "eswin,hsp_sp_csr");
	if (IS_ERR(dwc_priv->hsp_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(dwc_priv->hsp_regmap),
				"Failed to get hsp_sp_csr regmap\n");

	ret = of_property_read_u32_index(pdev->dev.of_node, "eswin,hsp_sp_csr", 2,
					 &eth_phy_ctrl_offset);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "can't get eth_phy_ctrl_offset\n");

	regmap_read(dwc_priv->hsp_regmap, eth_phy_ctrl_offset, &eth_phy_ctrl_regset);
	eth_phy_ctrl_regset |= (EIC7700_ETH_TX_CLK_SEL | EIC7700_ETH_PHY_INTF_SELI);
	regmap_write(dwc_priv->hsp_regmap, eth_phy_ctrl_offset, eth_phy_ctrl_regset);

	ret = of_property_read_u32_index(pdev->dev.of_node, "eswin,hsp_sp_csr", 3,
					 &eth_axi_lp_ctrl_offset);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "can't get eth_axi_lp_ctrl_offset\n");

	regmap_write(dwc_priv->hsp_regmap, eth_axi_lp_ctrl_offset, EIC7700_ETH_CSYSREQ_VAL);

	plat_dat->clk_tx_i = devm_clk_get_enabled(&pdev->dev, "tx");
	if (IS_ERR(plat_dat->clk_tx_i))
		return dev_err_probe(&pdev->dev, PTR_ERR(plat_dat->clk_tx_i),
				"error getting tx clock\n");

	plat_dat->fix_mac_speed = eic7700_qos_fix_speed;
	plat_dat->set_clk_tx_rate = stmmac_set_clk_tx_rate;
	plat_dat->bsp_priv = dwc_priv;

	ret = stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to driver probe\n");

	return ret;
}

static const struct of_device_id eic7700_dwmac_match[] = {
	{ .compatible = "eswin,eic7700-qos-eth" },
	{ }
};
MODULE_DEVICE_TABLE(of, eic7700_dwmac_match);

static struct platform_driver eic7700_dwmac_driver = {
	.probe  = eic7700_dwmac_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name           = "eic7700-eth-dwmac",
		.pm             = &stmmac_pltfr_pm_ops,
		.of_match_table = eic7700_dwmac_match,
	},
};
module_platform_driver(eic7700_dwmac_driver);

MODULE_AUTHOR("Eswin");
MODULE_AUTHOR("Shuang Liang <liangshuang@eswincomputing.com>");
MODULE_AUTHOR("Shangjuan Wei <weishangjuan@eswincomputing.com>");
