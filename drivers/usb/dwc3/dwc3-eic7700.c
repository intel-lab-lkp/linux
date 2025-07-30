// SPDX-License-Identifier: GPL-2.0
/*
 * Eswin Specific Glue layer
 *
 * Copyright 2025, Beijing ESWIN Computing Technology Co., Ltd.
 *
 * Authors: Wei Yang <yangwei1@eswincomputing.com>
 *          Senchuan Zhang <zhangsenchuan@eswincomputing.com>
 */

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/reset.h>
#include <linux/usb.h>
#include <linux/pm.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>

#define HSP_USB_VBUS_FSEL 0x2a
#define HSP_USB_MPLL_DEFAULT 0x0

#define HSP_USB_BUS_FILTER_EN (0x1 << 0)
#define HSP_USB_BUS_CLKEN_GM (0x1 << 9)
#define HSP_USB_BUS_CLKEN_GS (0x1 << 16)
#define HSP_USB_BUS_SW_RST (0x1 << 24)
#define HSP_USB_BUS_CLK_EN (0x1 << 28)

#define HSP_USB_AXI_LP_XM_CSYSREQ (0x1 << 0)
#define HSP_USB_AXI_LP_XS_CSYSREQ (0x1 << 16)

struct dwc3_eswin {
	int num_clks;
	struct device *dev;
	struct clk_bulk_data *clks;
	struct reset_control *vaux_rst;
};

static int dwc_usb_clk_init(struct device *dev)
{
	struct regmap *regmap;
	u32 hsp_usb_bus;
	u32 hsp_usb_axi_lp;
	u32 hsp_usb_vbus_freq;
	u32 hsp_usb_mpll;
	u32 args[4];

	regmap = syscon_regmap_lookup_by_phandle_args(dev->of_node,
						      "eswin,hsp-sp-csr", 4,
						      args);
	if (IS_ERR(regmap)) {
		dev_err(dev, "No hsp-sp-csr phandle specified\n");
		return PTR_ERR(regmap);
	}

	hsp_usb_bus       = args[0];
	hsp_usb_axi_lp    = args[1];
	hsp_usb_vbus_freq = args[2];
	hsp_usb_mpll      = args[3];

	/*
	 * usb clock init
	 * ref clock is 24M, below need to be set to satisfy
	 * usb phy requirement(125M)
	 */
	regmap_write(regmap, hsp_usb_vbus_freq, HSP_USB_VBUS_FSEL);
	regmap_write(regmap, hsp_usb_mpll, HSP_USB_MPLL_DEFAULT);
	/*
	 * reset usb core and usb phy
	 */
	regmap_write(regmap, hsp_usb_bus, HSP_USB_BUS_FILTER_EN |
		     HSP_USB_BUS_CLKEN_GM | HSP_USB_BUS_CLKEN_GS |
		     HSP_USB_BUS_SW_RST | HSP_USB_BUS_CLK_EN);
	regmap_write(regmap, hsp_usb_axi_lp, HSP_USB_AXI_LP_XM_CSYSREQ |
		     HSP_USB_AXI_LP_XS_CSYSREQ);

	return 0;
}

static int dwc3_eswin_probe(struct platform_device *pdev)
{
	struct dwc3_eswin *eswin;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int ret;

	eswin = devm_kzalloc(dev, sizeof(*eswin), GFP_KERNEL);
	if (!eswin)
		return -ENOMEM;

	eswin->dev = dev;
	platform_set_drvdata(pdev, eswin);

	eswin->num_clks = devm_clk_bulk_get_all_enabled(dev, &eswin->clks);
	if (eswin->num_clks < 0)
		return dev_err_probe(dev, eswin->num_clks,
				     "Failed to get usb clocks\n");

	eswin->vaux_rst = devm_reset_control_get(dev, "vaux");
	if (IS_ERR(eswin->vaux_rst))
		return dev_err_probe(dev, PTR_ERR(eswin->vaux_rst),
				     "Failed to get vaux reset\n");

	ret = reset_control_deassert(eswin->vaux_rst);
	if (ret)
		return dev_err_probe(eswin->dev, ret,
				     "Failed to deassert reset\n");

	ret = dwc_usb_clk_init(dev);
	if (ret) {
		dev_err(eswin->dev, "Failed to clk init: %d\n", ret);
		goto reset_assert;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "get_sync failed with err %d\n", ret);
		goto disable_pm;
	}

	/* Allocate and initialize the core */
	ret = of_platform_populate(np, NULL, NULL, dev);
	if (ret) {
		dev_err(dev, "failed to create dwc3 core: %d\n", ret);
		goto put_pm;
	}

	return ret;

put_pm:
	pm_runtime_put_sync(dev);
disable_pm:
	pm_runtime_disable(dev);
reset_assert:
	reset_control_assert(eswin->vaux_rst);

	return ret;
}

static void dwc3_eswin_remove(struct platform_device *pdev)
{
	struct dwc3_eswin *eswin = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	of_platform_depopulate(dev);

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
	reset_control_assert(eswin->vaux_rst);
}

static int __maybe_unused dwc3_eswin_runtime_suspend(struct device *dev)
{
	struct dwc3_eswin *eswin = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(eswin->num_clks, eswin->clks);

	return 0;
}

static int __maybe_unused dwc3_eswin_runtime_resume(struct device *dev)
{
	struct dwc3_eswin *eswin = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(eswin->num_clks, eswin->clks);
	if (ret) {
		dev_err(dev, "failed to enable clocks: %d\n", ret);
		return ret;
	}

	return ret;
}

static const struct dev_pm_ops dwc3_eswin_dev_pm_ops = {
	SET_RUNTIME_PM_OPS(dwc3_eswin_runtime_suspend,
			   dwc3_eswin_runtime_resume, NULL)
};

static const struct of_device_id eswin_dwc3_match[] = {
	{ .compatible = "eswin,eic7700-dwc3" },
	{},
};

MODULE_DEVICE_TABLE(of, eswin_dwc3_match);

static struct platform_driver dwc3_eswin_driver = {
	.probe = dwc3_eswin_probe,
	.remove = dwc3_eswin_remove,
	.driver = {
		.name = "eic7700-dwc3",
		.pm	= &dwc3_eswin_dev_pm_ops,
		.of_match_table = eswin_dwc3_match,
	},
};

module_platform_driver(dwc3_eswin_driver);

MODULE_AUTHOR("Wei Yang <yangwei1@eswincomputing.com");
MODULE_AUTHOR("Senchuan Zhang <zhangsenchuan@eswincomputing.com");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DesignWare USB3 ESWIN Glue Layer");
