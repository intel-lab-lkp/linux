// SPDX-License-Identifier: GPL-2.0
/*
 * ESWIN Specific Glue layer
 *
 * Copyright 2025, Beijing ESWIN Computing Technology Co., Ltd.
 *
 * Authors: Wei Yang <yangwei1@eswincomputing.com>
 *          Senchuan Zhang <zhangsenchuan@eswincomputing.com>
 *          Hang Cao <caohang@eswincomputing.com>
 */

#include <linux/clk.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/usb.h>

#include "glue.h"

#define HSP_USB_VBUS_FSEL		0x2a
#define HSP_USB_MPLL_DEFAULT		0x0

#define HSP_USB_BUS_FILTER_EN		BIT(0)
#define HSP_USB_BUS_CLKEN_GM		BIT(9)
#define HSP_USB_BUS_CLKEN_GS		BIT(16)
#define HSP_USB_BUS_SW_RST		BIT(24)
#define HSP_USB_BUS_CLK_EN		BIT(28)

#define HSP_USB_AXI_LP_XM_CSYSREQ	BIT(0)
#define HSP_USB_AXI_LP_XS_CSYSREQ	BIT(16)

struct dwc3_eswin {
	struct device *dev;
	struct dwc3 dwc;
	struct clk_bulk_data *clks;
	int num_clks;
	struct reset_control *vaux_rst;
};

#define to_dwc3_eswin(d) container_of((d), struct dwc3_eswin, dwc)

static int dwc_usb_clk_init(struct device *dev)
{
	struct regmap *regmap;
	u32 hsp_usb_vbus_freq;
	u32 hsp_usb_axi_lp;
	u32 hsp_usb_bus;
	u32 hsp_usb_mpll;
	u32 args[4];

	regmap = syscon_regmap_lookup_by_phandle_args(dev->of_node,
						      "eswin,hsp-sp-csr",
						      ARRAY_SIZE(args), args);
	if (IS_ERR(regmap)) {
		dev_err(dev, "No hsp-sp-csr phandle specified\n");
		return PTR_ERR(regmap);
	}

	hsp_usb_bus       = args[0];
	hsp_usb_axi_lp    = args[1];
	hsp_usb_vbus_freq = args[2];
	hsp_usb_mpll      = args[3];

	/*
	 * usb clock init,ref clock is 24M below need to be set to satisfy usb
	 * phy requirement(125M)
	 */
	regmap_write(regmap, hsp_usb_vbus_freq, HSP_USB_VBUS_FSEL);
	regmap_write(regmap, hsp_usb_mpll, HSP_USB_MPLL_DEFAULT);

	/* reset usb core and usb phy */
	regmap_write(regmap, hsp_usb_bus, HSP_USB_BUS_FILTER_EN |
		     HSP_USB_BUS_CLKEN_GM | HSP_USB_BUS_CLKEN_GS |
		     HSP_USB_BUS_SW_RST | HSP_USB_BUS_CLK_EN);
	regmap_write(regmap, hsp_usb_axi_lp, HSP_USB_AXI_LP_XM_CSYSREQ |
		     HSP_USB_AXI_LP_XS_CSYSREQ);

	return 0;
}

static int dwc3_eswin_probe(struct platform_device *pdev)
{
	struct dwc3_probe_data probe_data = {};
	struct device *dev = &pdev->dev;
	struct dwc3_eswin *eswin;
	struct resource *res;
	int ret;

	eswin = devm_kzalloc(dev, sizeof(*eswin), GFP_KERNEL);
	if (!eswin)
		return -ENOMEM;

	eswin->dev = dev;
	eswin->num_clks = devm_clk_bulk_get_all_enabled(dev, &eswin->clks);
	if (eswin->num_clks < 0)
		return dev_err_probe(dev, eswin->num_clks,
			"Failed to get usb clocks\n");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(dev, -ENODEV,
			"Missing memory resource\n");

	eswin->vaux_rst = devm_reset_control_get(dev, "vaux");
	if (IS_ERR(eswin->vaux_rst))
		return dev_err_probe(dev, PTR_ERR(eswin->vaux_rst),
		"Failed to get vaux reset\n");

	ret = reset_control_deassert(eswin->vaux_rst);
	if (ret)
		return dev_err_probe(dev, ret,
			"Failed to deassert reset\n");

	ret = dwc_usb_clk_init(dev);
	if (ret) {
		dev_err(dev, "Failed to clk init: %d\n", ret);
		goto reset_assert;
	}

	eswin->dwc.dev = dev;
	probe_data.dwc = &eswin->dwc;
	probe_data.res = res;
	probe_data.ignore_clocks_and_resets = true;
	ret = dwc3_core_probe(&probe_data);
	if (ret) {
		ret = dev_err_probe(dev, ret, "Failed to register DWC3 Core\n");
		goto reset_assert;
	}

	return 0;

reset_assert:
	reset_control_assert(eswin->vaux_rst);

	return ret;
}

static void dwc3_eswin_remove(struct platform_device *pdev)
{
	struct dwc3 *dwc = platform_get_drvdata(pdev);
	struct dwc3_eswin *eswin = to_dwc3_eswin(dwc);

	dwc3_core_remove(&eswin->dwc);

	reset_control_assert(eswin->vaux_rst);
}

static void dwc3_eswin_complete(struct device *dev)
{
	struct dwc3 *dwc = dev_get_drvdata(dev);

	dwc3_pm_complete(dwc);
}

static int dwc3_eswin_prepare(struct device *dev)
{
	struct dwc3 *dwc = dev_get_drvdata(dev);

	return dwc3_pm_prepare(dwc);
}

static int dwc3_eswin_pm_suspend(struct device *dev)
{
	struct dwc3 *dwc = dev_get_drvdata(dev);
	struct dwc3_eswin *eswin = to_dwc3_eswin(dwc);
	int ret;

	ret = dwc3_pm_suspend(&eswin->dwc);
	if (ret)
		return ret;

	clk_bulk_disable_unprepare(eswin->num_clks, eswin->clks);

	return 0;
}

static int dwc3_eswin_pm_resume(struct device *dev)
{
	struct dwc3 *dwc = dev_get_drvdata(dev);
	struct dwc3_eswin *eswin = to_dwc3_eswin(dwc);
	int ret;

	ret = clk_bulk_prepare_enable(eswin->num_clks, eswin->clks);
	if (ret) {
		dev_err(dev, "Failed to enable clocks: %d\n", ret);
		return ret;
	}

	return dwc3_pm_resume(&eswin->dwc);
}

static int dwc3_eswin_runtime_suspend(struct device *dev)
{
	struct dwc3 *dwc = dev_get_drvdata(dev);
	struct dwc3_eswin *eswin = to_dwc3_eswin(dwc);
	int ret;

	ret = dwc3_runtime_suspend(&eswin->dwc);
	if (ret)
		return ret;

	clk_bulk_disable_unprepare(eswin->num_clks, eswin->clks);
	return 0;
}

static int dwc3_eswin_runtime_resume(struct device *dev)
{
	struct dwc3 *dwc = dev_get_drvdata(dev);
	struct dwc3_eswin *eswin = to_dwc3_eswin(dwc);
	int ret;

	ret = clk_bulk_prepare_enable(eswin->num_clks, eswin->clks);
	if (ret) {
		dev_err(dev, "Failed to enable clocks: %d\n", ret);
		return ret;
	}

	return dwc3_runtime_resume(&eswin->dwc);
}

static int dwc3_eswin_runtime_idle(struct device *dev)
{
	return dwc3_runtime_idle(dev_get_drvdata(dev));
}

static const struct dev_pm_ops dwc3_eswin_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dwc3_eswin_pm_suspend, dwc3_eswin_pm_resume)
	SET_RUNTIME_PM_OPS(dwc3_eswin_runtime_suspend,
			   dwc3_eswin_runtime_resume, dwc3_eswin_runtime_idle)
	.complete = pm_sleep_ptr(dwc3_eswin_complete),
	.prepare = pm_sleep_ptr(dwc3_eswin_prepare),
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
		.pm	= pm_ptr(&dwc3_eswin_dev_pm_ops),
		.of_match_table = eswin_dwc3_match,
	},
};

module_platform_driver(dwc3_eswin_driver);

MODULE_AUTHOR("Wei Yang <yangwei1@eswincomputing.com");
MODULE_AUTHOR("Senchuan Zhang <zhangsenchuan@eswincomputing.com");
MODULE_AUTHOR("Hang Cao <caohang@eswincomputing.com");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DesignWare USB3 ESWIN Glue Layer");
