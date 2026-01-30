// SPDX-License-Identifier: GPL-2.0+
/*
 * Spacemit DWMAC platform driver
 *
 * Copyright (C) 2026 Inochi Amaoto <inochiama@gmail.com>
 */

#include <linux/clk.h>
#include <linux/math.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include "stmmac_platform.h"

/* ctrl register bits */
#define CTRL_PHY_INTF_RGMII		BIT(3)
#define CTRL_PHY_INTF_MII		BIT(4)
#define CTRL_WAKE_IRQ_EN		BIT(9)
#define CTRL_PHY_IRQ_EN			BIT(12)

/* dline register bits */
#define RGMII_RX_DLINE_EN		BIT(0)
#define RGMII_RX_DLINE_STEP		GENMASK(5, 4)
#define RGMII_RX_DLINE_CODE		GENMASK(15, 8)
#define RGMII_TX_DLINE_EN		BIT(16)
#define RGMII_TX_DLINE_STEP		GENMASK(21, 20)
#define RGMII_TX_DLINE_CODE		GENMASK(31, 24)

#define MAX_DLINE_DELAY_CODE		0xff
#define MAX_WORKED_DELAY		2800
/* Note: the delay step value is at 0.1ps */
#define K3_DELAY_STEP			367

static int spacemit_dwmac_set_delay(struct regmap *apmu, unsigned int dline_offset,
				    unsigned int tx_code, unsigned int rx_code)
{
	unsigned int mask, val;

	mask = RGMII_TX_DLINE_STEP | RGMII_TX_DLINE_CODE | RGMII_TX_DLINE_EN |
	       RGMII_RX_DLINE_STEP | RGMII_RX_DLINE_CODE | RGMII_RX_DLINE_EN;

	/*
	 * Since the delay step provided by config 0 is small enough, and
	 * it can cover the range of the valid delay, so there is no needed
	 * to use other step config.
	 */
	val = FIELD_PREP(RGMII_TX_DLINE_STEP, 0) |
	      FIELD_PREP(RGMII_TX_DLINE_CODE, tx_code) | RGMII_TX_DLINE_EN |
	      FIELD_PREP(RGMII_RX_DLINE_STEP, 0) |
	      FIELD_PREP(RGMII_RX_DLINE_CODE, rx_code) | RGMII_RX_DLINE_EN;

	return regmap_update_bits(apmu, dline_offset, mask, val);
}

static int spacemit_dwmac_detected_delay_value(unsigned int delay)
{
	if (delay == 0)
		return 0;

	if (delay > MAX_WORKED_DELAY)
		return -EINVAL;

	/*
	 * Note K3 require a specific factor for calculate
	 * the delay, in this scenario it is 0.9. So the
	 * formula is code * step / 10 * 0.9
	 */
	return DIV_ROUND_CLOSEST(delay * 10 * 10, K3_DELAY_STEP * 9);
}

static int spacemit_dwmac_fix_delay(struct plat_stmmacenet_data *plat_dat,
				    struct regmap *apmu,
				    unsigned int dline_offset,
				    unsigned int tx_delay, unsigned int rx_delay)
{
	int rx_code;
	int tx_code;

	rx_code = spacemit_dwmac_detected_delay_value(rx_delay);
	if (rx_code < 0)
		return rx_code;

	tx_code = spacemit_dwmac_detected_delay_value(tx_delay);
	if (tx_code < 0)
		return tx_code;

	return spacemit_dwmac_set_delay(apmu, dline_offset, tx_code, rx_code);
}

static int spacemit_dwmac_update_ifconfig(struct plat_stmmacenet_data *plat_dat,
					  struct stmmac_resources *stmmac_res,
					  struct regmap *apmu,
					  unsigned int ctrl_offset)
{
	unsigned int mask = CTRL_PHY_INTF_MII | CTRL_PHY_INTF_RGMII | CTRL_WAKE_IRQ_EN;
	unsigned int val = 0;

	switch (plat_dat->phy_interface) {
	case PHY_INTERFACE_MODE_MII:
		val = CTRL_PHY_INTF_MII;
		break;

	case PHY_INTERFACE_MODE_RMII:
		break;

	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		val = CTRL_PHY_INTF_RGMII;
		break;

	default:
		return -EOPNOTSUPP;
	}

	if (stmmac_res->wol_irq >= 0)
		val |= CTRL_WAKE_IRQ_EN;

	return regmap_update_bits(apmu, ctrl_offset, mask, val);
}

static int spacemit_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device *dev = &pdev->dev;
	unsigned int offset[2];
	struct regmap *apmu;
	struct clk *clk_tx;
	u32 rx_delay = 0;
	u32 tx_delay = 0;
	int ret;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to get platform resources\n");

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return dev_err_probe(dev, PTR_ERR(plat_dat),
				     "failed to parse DT parameters\n");

	clk_tx = devm_clk_get_enabled(&pdev->dev, "tx");
	if (IS_ERR(clk_tx))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk_tx),
				     "failed to get tx clock\n");

	apmu = syscon_regmap_lookup_by_phandle_args(pdev->dev.of_node, "spacemit,apmu", 2, offset);
	if (IS_ERR(apmu))
		return dev_err_probe(dev, PTR_ERR(apmu),
				"Failed to get apmu regmap\n");

	ret = spacemit_dwmac_update_ifconfig(plat_dat, &stmmac_res,
					     apmu, offset[0]);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to configure ifconfig\n");

	of_property_read_u32(pdev->dev.of_node, "tx-internal-delay-ps", &tx_delay);
	of_property_read_u32(pdev->dev.of_node, "rx-internal-delay-ps", &rx_delay);

	ret = spacemit_dwmac_fix_delay(plat_dat, apmu, offset[1], tx_delay, rx_delay);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to configure delay\n");

	return stmmac_dvr_probe(dev, plat_dat, &stmmac_res);
}

static const struct of_device_id spacemit_dwmac_match[] = {
	{ .compatible = "spacemit,k3-dwmac" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, spacemit_dwmac_match);

static struct platform_driver spacemit_dwmac_driver = {
	.probe  = spacemit_dwmac_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name = "spacemit-dwmac",
		.pm = &stmmac_pltfr_pm_ops,
		.of_match_table = spacemit_dwmac_match,
	},
};
module_platform_driver(spacemit_dwmac_driver);

MODULE_AUTHOR("Inochi Amaoto <inochiama@gmail.com>");
MODULE_DESCRIPTION("Spacemit DWMAC platform driver");
MODULE_LICENSE("GPL");
