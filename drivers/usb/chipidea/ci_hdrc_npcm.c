// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2023 Nuvoton Technology corporation.

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/usb/chipidea.h>
#include <linux/clk.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/io.h>
#include <linux/reset-controller.h>
#include <linux/of.h>

#include "ci.h"

struct npcm_udc_data {
	struct platform_device	*ci;
	struct clk		*core_clk;
	struct ci_hdrc_platform_data pdata;
};

static int npcm_udc_notify_event(struct ci_hdrc *ci, unsigned event)
{
	struct device *dev = ci->dev->parent;

	switch (event) {
	case CI_HDRC_CONTROLLER_RESET_EVENT:
		/* clear all mode bits */
		hw_write(ci, OP_USBMODE, 0xffffffff, 0x0);
		break;
	default:
		dev_dbg(dev, "unknown ci_hdrc event\n");
		break;
	}

	return 0;
}

static int npcm_udc_probe(struct platform_device *pdev)
{
	int ret;
	unsigned int args[3];
	struct regmap *gcr_regmap;
	struct npcm_udc_data *ci;
	struct platform_device *plat_ci;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;

	ci = devm_kzalloc(&pdev->dev, sizeof(*ci), GFP_KERNEL);
	if (!ci)
		return -ENOMEM;
	platform_set_drvdata(pdev, ci);

	ci->core_clk = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(ci->core_clk))
		return PTR_ERR(ci->core_clk);

	ret = clk_prepare_enable(ci->core_clk);
	if (ret) {
		dev_err(dev, "failed to enable the clock: %d\n", ret);
		return ret;
	}

	ci->pdata.name = dev_name(dev);
	ci->pdata.capoffset = DEF_CAPOFFSET;
	ci->pdata.flags	= CI_HDRC_REQUIRES_ALIGNED_DMA |
		CI_HDRC_FORCE_VBUS_ACTIVE_ALWAYS;
	ci->pdata.phy_mode = USBPHY_INTERFACE_MODE_UTMI;
	ci->pdata.notify_event = npcm_udc_notify_event;

	gcr_regmap = syscon_regmap_lookup_by_phandle_args(np, "nuvoton,sysgcr",
							  3, args);
	if (!IS_ERR(gcr_regmap))
		regmap_update_bits(gcr_regmap, args[0], args[1], args[2]);

	plat_ci = ci_hdrc_add_device(dev, pdev->resource, pdev->num_resources,
				     &ci->pdata);
	if (IS_ERR(plat_ci)) {
		ret = PTR_ERR(plat_ci);
		dev_err(dev, "failed to register HDRC NPCM device: %d\n", ret);
		goto clk_err;
	}

	pm_runtime_no_callbacks(dev);
	pm_runtime_enable(dev);

	return 0;

clk_err:
	clk_disable_unprepare(ci->core_clk);
	return ret;
}

static int npcm_udc_remove(struct platform_device *pdev)
{
	struct npcm_udc_data *ci = platform_get_drvdata(pdev);

	pm_runtime_disable(&pdev->dev);
	ci_hdrc_remove_device(ci->ci);
	clk_disable_unprepare(ci->core_clk);

	return 0;
}

static const struct of_device_id npcm_udc_dt_match[] = {
	{ .compatible = "nuvoton,npcm-udc", },
	{ }
};
MODULE_DEVICE_TABLE(of, npcm_udc_dt_match);

static struct platform_driver npcm_udc_driver = {
	.probe = npcm_udc_probe,
	.remove = npcm_udc_remove,
	.driver = {
		.name = "npcm_udc",
		.of_match_table = npcm_udc_dt_match,
	},
};

module_platform_driver(npcm_udc_driver);

MODULE_DESCRIPTION("NPCM USB device controller driver");
MODULE_AUTHOR("Tomer Maimon <tomer.maimon@nuvoton.com>");
MODULE_ALIAS("platform:npcm-udc");
MODULE_LICENSE("GPL v2");
