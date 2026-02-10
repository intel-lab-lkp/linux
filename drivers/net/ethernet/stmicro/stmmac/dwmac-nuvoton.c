// SPDX-License-Identifier: GPL-2.0+
/*
 * Nuvoton DWMAC specific glue layer
 *
 * Copyright (C) 2025 Nuvoton Technology Corp.
 *
 * Author: Joey Lu <a0987203069@gmail.com>
 */

#include <linux/mfd/syscon.h>
#include <linux/of_device.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/stmmac.h>

#include "stmmac.h"
#include "stmmac_platform.h"

#define NVT_REG_SYS_GMAC0MISCR  0x108
#define NVT_REG_SYS_GMAC1MISCR  0x10C

#define NVT_MISCR_RMII          BIT(0)

/* Two thousand picoseconds are evenly mapped to a 4-bit field,
 * resulting in each step being 2000/15 picoseconds.
 */
#define NVT_PATH_DELAY_STEP     134
#define NVT_TX_DELAY_MASK       GENMASK(19, 16)
#define NVT_RX_DELAY_MASK       GENMASK(23, 20)

static int nvt_gmac_get_delay(struct device *dev, const char *property)
{
	u32 arg;

	if (of_property_read_u32(dev->of_node, property, &arg))
		return 0;

	if (arg > 2000) {
		dev_err(dev, "Invalid %s argument.\n", property);
		return -EINVAL;
	}

	if (arg == 2000)
		return 15;

	return arg / NVT_PATH_DELAY_STEP;
}

static int nvt_gmac_setup(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	phy_interface_t phy_mode;
	struct regmap *regmap;
	u32 macid, miscr, reg;
	int ret;

	regmap = syscon_regmap_lookup_by_phandle_args(dev->of_node, "nuvoton,sys", 1, &macid);
	if (IS_ERR(regmap))
		ret = dev_err_probe(dev, PTR_ERR(regmap), "Failed to get sys register\n");

	if (macid > 1)
		ret = dev_err_probe(dev, -EINVAL, "Invalid sys arguments\n");

	if (of_get_phy_mode(pdev->dev.of_node, &phy_mode))
		ret = dev_err_probe(dev, -EINVAL, "Missing phy mode property\n");

	miscr = (macid == 0) ? NVT_REG_SYS_GMAC0MISCR : NVT_REG_SYS_GMAC1MISCR;

	switch (phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		ret = nvt_gmac_get_delay(dev, "rx-internal-delay-ps");
		if (ret < 0)
			return ret;
		reg = FIELD_PREP(NVT_RX_DELAY_MASK, ret);

		ret = nvt_gmac_get_delay(dev, "tx-internal-delay-ps");
		if (ret < 0)
			return ret;
		reg |= FIELD_PREP(NVT_TX_DELAY_MASK, ret);
		break;
	case PHY_INTERFACE_MODE_RMII:
		reg = NVT_MISCR_RMII;
			break;
	default:
		return dev_err_probe(dev, -EINVAL, "Unsupported phy-mode (%d)\n", phy_mode);
	}

	regmap_update_bits(regmap, miscr,
			   NVT_RX_DELAY_MASK | NVT_TX_DELAY_MASK | NVT_MISCR_RMII, reg);

	return 0;
}

static int nvt_gmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device *dev = &pdev->dev;
	int ret;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get platform resources\n");

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return dev_err_probe(dev, PTR_ERR(plat_dat), "Failed to get platform data\n");

	ret = nvt_gmac_setup(pdev);
	if (ret)
		return ret;

	return stmmac_pltfr_probe(pdev, plat_dat, &stmmac_res);
}

static const struct of_device_id nvt_dwmac_match[] = {
	{ .compatible = "nuvoton,ma35d1-dwmac"},
	{ }
};
MODULE_DEVICE_TABLE(of, nvt_dwmac_match);

static struct platform_driver nvt_dwmac_driver = {
	.probe  = nvt_gmac_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name           = "nuvoton-dwmac",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = nvt_dwmac_match,
	},
};
module_platform_driver(nvt_dwmac_driver);

MODULE_AUTHOR("Joey Lu <a0987203069@gmail.com>");
MODULE_DESCRIPTION("Nuvoton DWMAC specific glue layer");
MODULE_LICENSE("GPL");
