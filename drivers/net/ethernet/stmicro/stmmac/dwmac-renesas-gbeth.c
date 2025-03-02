// SPDX-License-Identifier: GPL-2.0+
/*
 * dwmac-renesas-gbeth.c - DWMAC Specific Glue layer for Renesas GBETH
 *
 * Copyright (C) 2025 Renesas Electronics Corporation
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "dwmac4.h"
#include "stmmac_platform.h"

struct renesas_gbeth {
	struct device *dev;
	void __iomem *regs;
	unsigned int num_clks;
	struct clk_bulk_data *clks;
};

static const char *const renesas_gbeth_clks[] __initconst = {
	"rx", "rx-180", "tx-180",
};

static int renesas_gbeth_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device *dev = &pdev->dev;
	struct renesas_gbeth *gbeth;
	struct reset_control *rstc;
	unsigned int i;
	int err;

	err = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (err)
		return dev_err_probe(dev, err,
				     "failed to get resources\n");

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return dev_err_probe(dev, PTR_ERR(plat_dat),
				     "dt configuration failed\n");

	gbeth = devm_kzalloc(dev, sizeof(*gbeth), GFP_KERNEL);
	if (!gbeth)
		return -ENOMEM;

	plat_dat->clk_tx_i = devm_clk_get_enabled(dev, "tx");
	if (IS_ERR(plat_dat->clk_tx_i))
		return dev_err_probe(dev, PTR_ERR(plat_dat->clk_tx_i),
				     "error getting tx clock\n");

	gbeth->num_clks = ARRAY_SIZE(renesas_gbeth_clks);
	gbeth->clks = devm_kcalloc(dev, gbeth->num_clks,
				   sizeof(*gbeth->clks), GFP_KERNEL);
	if (!gbeth->clks)
		return -ENOMEM;

	for (i = 0; i <  gbeth->num_clks; i++)
		gbeth->clks[i].id = renesas_gbeth_clks[i];

	err = devm_clk_bulk_get(dev, gbeth->num_clks, gbeth->clks);
	if (err < 0)
		return err;

	err = clk_bulk_prepare_enable(gbeth->num_clks, gbeth->clks);
	if (err)
		return err;

	rstc = devm_reset_control_get_exclusive_deasserted(dev, NULL);
	if (IS_ERR(rstc))
		return PTR_ERR(rstc);

	gbeth->dev = dev;
	gbeth->regs = stmmac_res.addr;
	plat_dat->bsp_priv = gbeth;
	plat_dat->set_clk_tx_rate = stmmac_set_clk_tx_rate;
	plat_dat->flags |= STMMAC_FLAG_HWTSTAMP_CORRECT_LATENCY |
			   STMMAC_FLAG_EN_TX_LPI_CLOCKGATING |
			   STMMAC_FLAG_RX_CLK_RUNS_IN_LPI |
			   STMMAC_FLAG_SPH_DISABLE;

	return stmmac_dvr_probe(dev, plat_dat, &stmmac_res);
}

static void renesas_gbeth_remove(struct platform_device *pdev)
{
	struct renesas_gbeth *gbeth = get_stmmac_bsp_priv(&pdev->dev);

	stmmac_dvr_remove(&pdev->dev);

	clk_bulk_disable_unprepare(gbeth->num_clks, gbeth->clks);
}

static const struct of_device_id renesas_gbeth_match[] = {
	{ .compatible = "renesas,rzv2h-gbeth", },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, renesas_gbeth_match);

static struct platform_driver renesas_gbeth_driver = {
	.probe  = renesas_gbeth_probe,
	.remove = renesas_gbeth_remove,
	.driver = {
		.name		= "renesas-gbeth",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table	= renesas_gbeth_match,
	},
};
module_platform_driver(renesas_gbeth_driver);

MODULE_AUTHOR("Lad Prabhakar <prabhakar.mahadev-lad.rj@bp.renesas.com>");
MODULE_DESCRIPTION("Renesas GBETH DWMAC Specific Glue layer");
MODULE_LICENSE("GPL");
