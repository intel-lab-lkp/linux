// SPDX-License-Identifier: GPL-2.0-only
/*
 * dwmac-sun55i.c - Allwinner sun55i GMAC200 specific glue layer
 *
 * Copyright (C) 2025 Chen-Yu Tsai <wens@csie.org>
 *
 * syscon parts taken from dwmac-sun8i.c, which is
 *
 * Copyright (C) 2017 Corentin Labbe <clabbe.montjoie@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/stmmac.h>

#include "stmmac.h"
#include "stmmac_platform.h"

/* RMII specific bits */
#define SYSCON_RMII_EN		BIT(13) /* 1: enable RMII (overrides EPIT) */
/* Generic system control EMAC_CLK bits */
#define SYSCON_ETXDC_MASK		GENMASK(12, 10)
#define SYSCON_ERXDC_MASK		GENMASK(9, 5)
/* EMAC PHY Interface Type */
#define SYSCON_EPIT			BIT(2) /* 1: RGMII, 0: MII */
#define SYSCON_ETCS_MASK		GENMASK(1, 0)
#define SYSCON_ETCS_MII		0x0
#define SYSCON_ETCS_EXT_GMII	0x1
#define SYSCON_ETCS_INT_GMII	0x2

struct sun55i_gmac;

struct sun55i_gmac_data {
	int (*init_resources)(struct platform_device *pdev,
			      struct sun55i_gmac *gmac);
	unsigned int flags;
	u32 txdly_step_ps;
	u32 rxdly_step_ps;
	u32 offset;
};

struct sun55i_gmac {
	const struct sun55i_gmac_data *data;
	struct regmap *regmap;
};

static int sun55i_gmac200_init_resources(struct platform_device *pdev,
					 struct sun55i_gmac *gmac)
{
	gmac->regmap = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						       "syscon");
	if (IS_ERR(gmac->regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(gmac->regmap),
				     "Unable to map syscon\n");

	return 0;
}

static int sun55i_gmac200_validate_delay(struct device *dev, const char *dir,
					 u32 *val, u32 step, u32 max)
{
	if (*val % step)
		return dev_err_probe(dev, -EINVAL,
				     "%s-delay must be a multiple of %ups\n", dir, step);
	max *= step;
	if (*val > max)
		return dev_err_probe(dev, -EINVAL,
				     "%s clock delay exceeds maximum (%ups > %ups)\n",
				     dir, *val, max);
	*val /= step;
	dev_dbg(dev, "set %s-delay to %x\n", dir, *val);

	return 0;
}

static int sun55i_gmac200_setup(struct device *dev,
				struct plat_stmmacenet_data *plat,
				const struct sun55i_gmac *gmac)
{
	struct device_node *node = dev->of_node;
	u32 val, step, max, reg = 0;
	int ret;

	if (!of_property_read_u32(node, "tx-internal-delay-ps", &val)) {
		step = gmac->data->txdly_step_ps;
		max = FIELD_MAX(SYSCON_ETXDC_MASK);
		ret = sun55i_gmac200_validate_delay(dev, "tx", &val, step, max);
		if (ret)
			return ret;

		reg |= FIELD_PREP(SYSCON_ETXDC_MASK, val);
	}

	if (!of_property_read_u32(node, "rx-internal-delay-ps", &val)) {
		step = gmac->data->rxdly_step_ps;
		max = FIELD_MAX(SYSCON_ERXDC_MASK);
		ret = sun55i_gmac200_validate_delay(dev, "rx", &val, step, max);
		if (ret)
			return ret;

		reg |= FIELD_PREP(SYSCON_ERXDC_MASK, val);
	}

	switch (plat->phy_interface) {
	case PHY_INTERFACE_MODE_MII:
		/* default */
		break;
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		reg |= SYSCON_EPIT | SYSCON_ETCS_INT_GMII;
		break;
	case PHY_INTERFACE_MODE_RMII:
		reg |= SYSCON_RMII_EN;
		break;
	default:
		return dev_err_probe(dev, -EINVAL, "Unsupported interface mode: %s",
				     phy_modes(plat->phy_interface));
	}

	ret = regmap_write(gmac->regmap, gmac->data->offset, reg);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to write to syscon\n");

	return 0;
}

static int sun55i_gmac200_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device *dev = &pdev->dev;
	struct sun55i_gmac *gmac;
	struct clk *clk;
	int ret;

	gmac = devm_kzalloc(dev, sizeof(*gmac), GFP_KERNEL);
	if (!gmac)
		return -ENOMEM;

	gmac->data = device_get_match_data(dev);
	if (!gmac->data)
		return -EINVAL;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	ret = gmac->data->init_resources(pdev, gmac);
	if (ret)
		return ret;

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	plat_dat->flags |= gmac->data->flags;
	plat_dat->host_dma_width = 32;

	ret = sun55i_gmac200_setup(dev, plat_dat, gmac);
	if (ret)
		return ret;

	clk = devm_clk_get_enabled(dev, "mbus");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Failed to get or enable MBUS clock\n");

	ret = devm_regulator_get_enable_optional(dev, "phy");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get or enable PHY supply\n");

	return devm_stmmac_pltfr_probe(pdev, plat_dat, &stmmac_res);
}

static const struct sun55i_gmac_data sun55i_a523_gmac200_data = {
	.init_resources = sun55i_gmac200_init_resources,
	.flags = STMMAC_FLAG_SPH_DISABLE,
	.offset = 0x34,
	/*
	 * The actual delay steps reported by the manufacturer are:
	 * - Tx: 180ps
	 * - Rx: 530ps
	 *
	 * Correcting them would break the DT ABI of the published DTs.
	 */
	.txdly_step_ps = 100,
	.rxdly_step_ps = 100,
};

static const struct of_device_id sun55i_gmac200_match[] = {
	{ .compatible = "allwinner,sun55i-a523-gmac200",
	  .data = &sun55i_a523_gmac200_data },
	{ }
};
MODULE_DEVICE_TABLE(of, sun55i_gmac200_match);

static struct platform_driver sun55i_gmac200_driver = {
	.probe  = sun55i_gmac200_probe,
	.driver = {
		.name           = "dwmac-sun55i",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = sun55i_gmac200_match,
	},
};
module_platform_driver(sun55i_gmac200_driver);

MODULE_AUTHOR("Chen-Yu Tsai <wens@csie.org>");
MODULE_DESCRIPTION("Allwinner sun55i GMAC200 specific glue layer");
MODULE_LICENSE("GPL");
