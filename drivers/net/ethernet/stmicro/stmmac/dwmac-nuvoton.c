// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nuvoton DWMAC specific glue layer
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 *
 * Author: Joey Lu <yclu4@nuvoton.com>
 */

#include <linux/mfd/syscon.h>
#include <linux/of_device.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/stmmac.h>

#include "stmmac.h"
#include "stmmac_platform.h"

#define REG_SYS_GMAC0MISCR  0x108
#define REG_SYS_GMAC1MISCR  0x10C

#define MISCR_RMII          BIT(0)

/* 2000ps is mapped to 0 ~ 0xF */
#define PATH_DELAY_DEC      134
#define TX_DELAY_OFFSET     16
#define TX_DELAY_MASK       GENMASK(19, 16)
#define RX_DELAY_OFFSET     20
#define RX_DELAY_MASK       GENMASK(23, 20)

struct nvt_priv_data {
	struct platform_device *pdev;
	struct regmap *regmap;
};

static struct nvt_priv_data *
nuvoton_gmac_setup(struct platform_device *pdev, struct plat_stmmacenet_data *plat)
{
	struct device *dev = &pdev->dev;
	struct nvt_priv_data *bsp_priv;
	phy_interface_t phy_mode;
	u32 tx_delay, rx_delay;
	u32 macid, arg, reg;

	bsp_priv = devm_kzalloc(dev, sizeof(*bsp_priv), GFP_KERNEL);
	if (!bsp_priv)
		return ERR_PTR(-ENOMEM);

	bsp_priv->regmap =
		syscon_regmap_lookup_by_phandle_args(dev->of_node, "nuvoton,sys", 1, &macid);
	if (IS_ERR(bsp_priv->regmap)) {
		dev_err(dev, "Failed to get sys register\n");
		return ERR_PTR(-ENODEV);
	}
	if (macid > 1) {
		dev_err(dev, "Invalid sys arguments\n");
		return ERR_PTR(-EINVAL);
	}

	if (of_property_read_u32(dev->of_node, "tx-internal-delay-ps", &arg)) {
		tx_delay = 0; /* Default value is 0 */
	} else {
		if (arg <= 2000) {
			tx_delay = (arg == 2000) ? 0xF : (arg / PATH_DELAY_DEC);
			dev_dbg(dev, "Set Tx path delay to 0x%x\n", tx_delay);
		} else {
			dev_err(dev, "Invalid Tx path delay argument.\n");
			return ERR_PTR(-EINVAL);
		}
	}
	if (of_property_read_u32(dev->of_node, "rx-internal-delay-ps", &arg)) {
		rx_delay = 0; /* Default value is 0 */
	} else {
		if (arg <= 2000) {
			rx_delay = (arg == 2000) ? 0xF : (arg / PATH_DELAY_DEC);
			dev_dbg(dev, "Set Rx path delay to 0x%x\n", rx_delay);
		} else {
			dev_err(dev, "Invalid Rx path delay argument.\n");
			return ERR_PTR(-EINVAL);
		}
	}

	regmap_read(bsp_priv->regmap,
		    macid == 0 ? REG_SYS_GMAC0MISCR : REG_SYS_GMAC1MISCR, &reg);
	reg &= ~(TX_DELAY_MASK | RX_DELAY_MASK);

	if (of_get_phy_mode(pdev->dev.of_node, &phy_mode)) {
		dev_err(dev, "missing phy mode property\n");
		return ERR_PTR(-EINVAL);
	}

	switch (phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		reg &= ~MISCR_RMII;
		break;
	case PHY_INTERFACE_MODE_RMII:
		reg |= MISCR_RMII;
		break;
	default:
		dev_err(dev, "Unsupported phy-mode (%d)\n", phy_mode);
		return ERR_PTR(-EINVAL);
	}

	if (!(reg & MISCR_RMII)) {
		reg |= tx_delay << TX_DELAY_OFFSET;
		reg |= rx_delay << RX_DELAY_OFFSET;
	}

	regmap_write(bsp_priv->regmap,
		     macid == 0 ? REG_SYS_GMAC0MISCR : REG_SYS_GMAC1MISCR, reg);

	bsp_priv->pdev = pdev;

	return bsp_priv;
}

static int nuvoton_gmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	int ret;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	/* Nuvoton DWMAC configs */
	plat_dat->has_gmac = 1;
	plat_dat->tx_fifo_size = 2048;
	plat_dat->rx_fifo_size = 4096;
	plat_dat->multicast_filter_bins = 0;
	plat_dat->unicast_filter_entries = 8;
	plat_dat->flags &= ~STMMAC_FLAG_USE_PHY_WOL;

	plat_dat->bsp_priv = nuvoton_gmac_setup(pdev, plat_dat);
	if (IS_ERR(plat_dat->bsp_priv)) {
		ret = PTR_ERR(plat_dat->bsp_priv);
		return ret;
	}

	ret = stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
	if (ret)
		return ret;

	/* We support WoL by magic packet, override pmt to make it work! */
	plat_dat->pmt = 1;
	dev_info(&pdev->dev, "Wake-Up On Lan supported\n");
	device_set_wakeup_capable(&pdev->dev, 1);

	return 0;
}

static const struct of_device_id nuvoton_dwmac_match[] = {
	{ .compatible = "nuvoton,ma35d1-dwmac"},
	{ }
};
MODULE_DEVICE_TABLE(of, nuvoton_dwmac_match);

static struct platform_driver nuvoton_dwmac_driver = {
	.probe  = nuvoton_gmac_probe,
	.remove_new = stmmac_pltfr_remove,
	.driver = {
		.name           = "nuvoton-dwmac",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = nuvoton_dwmac_match,
	},
};
module_platform_driver(nuvoton_dwmac_driver);

MODULE_AUTHOR("Joey Lu <yclu4@nuvoton.com>");
MODULE_DESCRIPTION("Nuvoton DWMAC specific glue layer");
MODULE_LICENSE("GPL v2");
