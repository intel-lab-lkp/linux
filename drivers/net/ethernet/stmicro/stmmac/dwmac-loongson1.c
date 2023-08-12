// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Loongson-1 DWMAC glue layer
 *
 * Copyright (C) 2011-2023 Keguang Zhang <keguang.zhang@gmail.com>
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "stmmac.h"
#include "stmmac_platform.h"

/* Loongson-1 SYSCON Registers */
#define LS1X_SYSCON0		(0x0)
#define LS1X_SYSCON1		(0x4)

struct ls1x_dwmac_syscon {
	const struct reg_field *reg_fields;
	unsigned int nr_reg_fields;
	int (*syscon_init)(struct plat_stmmacenet_data *plat);
};

struct ls1x_dwmac {
	struct device *dev;
	struct plat_stmmacenet_data *plat_dat;
	const struct ls1x_dwmac_syscon *syscon;
	struct regmap *regmap;
	struct regmap_field *regmap_fields[];
};

enum ls1b_dwmac_syscon_regfield {
	GMAC1_USE_UART1,
	GMAC1_USE_UART0,
	GMAC1_SHUT,
	GMAC0_SHUT,
	GMAC1_USE_TXCLK,
	GMAC0_USE_TXCLK,
	GMAC1_USE_PWM23,
	GMAC0_USE_PWM01,
};

enum ls1c_dwmac_syscon_regfield {
	GMAC_SHUT,
	PHY_INTF_SELI,
};

const struct reg_field ls1b_dwmac_syscon_regfields[] = {
	[GMAC1_USE_UART1]	= REG_FIELD(LS1X_SYSCON0, 4, 4),
	[GMAC1_USE_UART0]	= REG_FIELD(LS1X_SYSCON0, 3, 3),
	[GMAC1_SHUT]		= REG_FIELD(LS1X_SYSCON1, 13, 13),
	[GMAC0_SHUT]		= REG_FIELD(LS1X_SYSCON1, 12, 12),
	[GMAC1_USE_TXCLK]	= REG_FIELD(LS1X_SYSCON1, 3, 3),
	[GMAC0_USE_TXCLK]	= REG_FIELD(LS1X_SYSCON1, 2, 2),
	[GMAC1_USE_PWM23]	= REG_FIELD(LS1X_SYSCON1, 1, 1),
	[GMAC0_USE_PWM01]	= REG_FIELD(LS1X_SYSCON1, 0, 0)
};

const struct reg_field ls1c_dwmac_syscon_regfields[] = {
	[GMAC_SHUT]		= REG_FIELD(LS1X_SYSCON0, 6, 6),
	[PHY_INTF_SELI]		= REG_FIELD(LS1X_SYSCON1, 28, 30)
};

static int ls1b_dwmac_syscon_init(struct plat_stmmacenet_data *plat)
{
	struct ls1x_dwmac *dwmac = plat->bsp_priv;
	struct regmap_field **regmap_fields = dwmac->regmap_fields;

	if (plat->bus_id) {
		regmap_field_write(regmap_fields[GMAC1_USE_UART1], 1);
		regmap_field_write(regmap_fields[GMAC1_USE_UART0], 1);

		switch (plat->phy_interface) {
		case PHY_INTERFACE_MODE_RGMII:
			regmap_field_write(regmap_fields[GMAC1_USE_TXCLK], 0);
			regmap_field_write(regmap_fields[GMAC1_USE_PWM23], 0);
			break;
		case PHY_INTERFACE_MODE_MII:
			regmap_field_write(regmap_fields[GMAC1_USE_TXCLK], 1);
			regmap_field_write(regmap_fields[GMAC1_USE_PWM23], 1);
			break;
		default:
			dev_err(dwmac->dev, "Unsupported PHY mode %u\n",
				plat->phy_interface);
			return -EOPNOTSUPP;
		}

		regmap_field_write(regmap_fields[GMAC1_SHUT], 0);
	} else {
		switch (plat->phy_interface) {
		case PHY_INTERFACE_MODE_RGMII:
			regmap_field_write(regmap_fields[GMAC0_USE_TXCLK], 0);
			regmap_field_write(regmap_fields[GMAC0_USE_PWM01], 0);
			break;
		case PHY_INTERFACE_MODE_MII:
			regmap_field_write(regmap_fields[GMAC0_USE_TXCLK], 1);
			regmap_field_write(regmap_fields[GMAC0_USE_PWM01], 1);
			break;
		default:
			dev_err(dwmac->dev, "Unsupported PHY mode %u\n",
				plat->phy_interface);
			return -EOPNOTSUPP;
		}

		regmap_field_write(regmap_fields[GMAC0_SHUT], 0);
	}

	return 0;
}

static int ls1c_dwmac_syscon_init(struct plat_stmmacenet_data *plat)
{
	struct ls1x_dwmac *dwmac = plat->bsp_priv;
	struct regmap_field **regmap_fields = dwmac->regmap_fields;

	if (plat->phy_interface == PHY_INTERFACE_MODE_RMII) {
		regmap_field_write(regmap_fields[PHY_INTF_SELI], 0x4);
	} else {
		dev_err(dwmac->dev, "Unsupported PHY-mode %u\n",
			plat->phy_interface);
		return -EOPNOTSUPP;
	}

	regmap_field_write(regmap_fields[GMAC_SHUT], 0);

	return 0;
}

static const struct ls1x_dwmac_syscon ls1b_dwmac_syscon = {
	.reg_fields = ls1b_dwmac_syscon_regfields,
	.nr_reg_fields = ARRAY_SIZE(ls1b_dwmac_syscon_regfields),
	.syscon_init = ls1b_dwmac_syscon_init,
};

static const struct ls1x_dwmac_syscon ls1c_dwmac_syscon = {
	.reg_fields = ls1c_dwmac_syscon_regfields,
	.nr_reg_fields = ARRAY_SIZE(ls1c_dwmac_syscon_regfields),
	.syscon_init = ls1c_dwmac_syscon_init,
};

static int ls1x_dwmac_init(struct platform_device *pdev, void *priv)
{
	struct ls1x_dwmac *dwmac = priv;
	int ret;

	ret = devm_regmap_field_bulk_alloc(dwmac->dev, dwmac->regmap,
					   dwmac->regmap_fields,
					   dwmac->syscon->reg_fields,
					   dwmac->syscon->nr_reg_fields);
	if (ret)
		return ret;

	if (dwmac->syscon->syscon_init) {
		ret = dwmac->syscon->syscon_init(dwmac->plat_dat);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct of_device_id ls1x_dwmac_syscon_match[] = {
	{ .compatible = "loongson,ls1b-syscon", .data = &ls1b_dwmac_syscon },
	{ .compatible = "loongson,ls1c-syscon", .data = &ls1c_dwmac_syscon },
	{ }
};

static int ls1x_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device_node *syscon_np;
	const struct of_device_id *match;
	struct regmap *regmap;
	struct ls1x_dwmac *dwmac;
	const struct ls1x_dwmac_syscon *syscon;
	size_t size;
	int ret;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	/* Probe syscon */
	syscon_np = of_parse_phandle(pdev->dev.of_node, "syscon", 0);
	if (!syscon_np)
		return -ENODEV;

	match = of_match_node(ls1x_dwmac_syscon_match, syscon_np);
	if (!match) {
		of_node_put(syscon_np);
		return -EINVAL;
	}
	syscon = (const struct ls1x_dwmac_syscon *)match->data;

	regmap = syscon_node_to_regmap(syscon_np);
	of_node_put(syscon_np);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(&pdev->dev, "Unable to map syscon: %d\n", ret);
		return ret;
	}

	size = syscon->nr_reg_fields * sizeof(struct regmap_field *);
	dwmac = devm_kzalloc(&pdev->dev, sizeof(*dwmac) + size, GFP_KERNEL);
	if (!dwmac)
		return -ENOMEM;

	plat_dat = stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat)) {
		dev_err(&pdev->dev, "dt configuration failed\n");
		return PTR_ERR(plat_dat);
	}

	plat_dat->bsp_priv = dwmac;
	plat_dat->init = ls1x_dwmac_init;
	dwmac->dev = &pdev->dev;
	dwmac->plat_dat = plat_dat;
	dwmac->syscon = syscon;
	dwmac->regmap = regmap;

	ret = stmmac_pltfr_probe(pdev, plat_dat, &stmmac_res);
	if (ret)
		goto err_remove_config_dt;

	return 0;

err_remove_config_dt:
	if (pdev->dev.of_node)
		stmmac_remove_config_dt(pdev, plat_dat);

	return ret;
}

static const struct of_device_id ls1x_dwmac_match[] = {
	{ .compatible = "loongson,ls1b-dwmac" },
	{ .compatible = "loongson,ls1c-dwmac" },
	{ }
};
MODULE_DEVICE_TABLE(of, ls1x_dwmac_match);

static struct platform_driver ls1x_dwmac_driver = {
	.probe = ls1x_dwmac_probe,
	.remove_new = stmmac_pltfr_remove,
	.driver = {
		.name = "loongson1-dwmac",
		.of_match_table = ls1x_dwmac_match,
	},
};
module_platform_driver(ls1x_dwmac_driver);

MODULE_AUTHOR("Keguang Zhang <keguang.zhang@gmail.com>");
MODULE_DESCRIPTION("Loongson1 DWMAC glue layer");
MODULE_LICENSE("GPL");
