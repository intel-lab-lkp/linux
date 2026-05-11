// SPDX-License-Identifier: GPL-2.0
/*
 * cdnsp-sky1.c - CIX Sky1 glue for Cadence USBSSP DRD controller
 *
 * Copyright (C) 2026 CIX Technology Group Co., Ltd.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include "glue.h"

#define AXI_SETTING_OFFSET		0x0
/* Normal Non-cacheable Bufferable */
#define SKY1_USB_AXI_WR_CACHE_VALUE	0x33

/* USB mode strap in S5 syscon */
#define USB_MODE_STRAP_S5_DOMAIN	0x424
#define MODE_STRAP_OTG			0

#define U3_TYPEC_DRD_ID			0
#define U3_TYPEC_HOST0_ID		1
#define U3_TYPEC_HOST1_ID		2
#define U3_TYPEC_HOST2_ID		3
#define U3_TYPEA_CTRL0_ID		4
#define U3_TYPEA_CTRL1_ID		5
#define U2_HOST0_ID			6
#define U2_HOST1_ID			7
#define U2_HOST2_ID			8
#define U2_HOST3_ID			9
#define SKY1_USB_S5_NUM			10

#define U3_TYPEC_DRD_MODE_STRAP_BIT		12
#define U3_TYPEC_HOST0_MODE_STRAP_BIT		14
#define U3_TYPEC_HOST1_MODE_STRAP_BIT		16
#define U3_TYPEC_HOST2_MODE_STRAP_BIT		18
#define U3_TYPEA_CTRL0_MODE_STRAP_BIT		8
#define U3_TYPEA_CTRL1_MODE_STRAP_BIT		10
#define U2_HOST0_MODE_STRAP_BIT			0
#define U2_HOST1_MODE_STRAP_BIT			2
#define U2_HOST2_MODE_STRAP_BIT			4
#define U2_HOST3_MODE_STRAP_BIT			6

struct cdnsp_sky1_strap_signal {
	unsigned int offset, bit;
};

static const struct cdnsp_sky1_strap_signal strap_signals[SKY1_USB_S5_NUM] = {
	[U3_TYPEC_DRD_ID]	= { USB_MODE_STRAP_S5_DOMAIN, U3_TYPEC_DRD_MODE_STRAP_BIT },
	[U3_TYPEC_HOST0_ID]	= { USB_MODE_STRAP_S5_DOMAIN, U3_TYPEC_HOST0_MODE_STRAP_BIT },
	[U3_TYPEC_HOST1_ID]	= { USB_MODE_STRAP_S5_DOMAIN, U3_TYPEC_HOST1_MODE_STRAP_BIT },
	[U3_TYPEC_HOST2_ID]	= { USB_MODE_STRAP_S5_DOMAIN, U3_TYPEC_HOST2_MODE_STRAP_BIT },
	[U3_TYPEA_CTRL0_ID]	= { USB_MODE_STRAP_S5_DOMAIN, U3_TYPEA_CTRL0_MODE_STRAP_BIT },
	[U3_TYPEA_CTRL1_ID]	= { USB_MODE_STRAP_S5_DOMAIN, U3_TYPEA_CTRL1_MODE_STRAP_BIT },
	[U2_HOST0_ID]		= { USB_MODE_STRAP_S5_DOMAIN, U2_HOST0_MODE_STRAP_BIT },
	[U2_HOST1_ID]		= { USB_MODE_STRAP_S5_DOMAIN, U2_HOST1_MODE_STRAP_BIT },
	[U2_HOST2_ID]		= { USB_MODE_STRAP_S5_DOMAIN, U2_HOST2_MODE_STRAP_BIT },
	[U2_HOST3_ID]		= { USB_MODE_STRAP_S5_DOMAIN, U2_HOST3_MODE_STRAP_BIT },
};

struct cdnsp_sky1 {
	struct device *dev;
	struct cdns cdns;
	struct regmap *usb_syscon;
	void __iomem *glue_base;
	struct clk_bulk_data *clks;
	int num_clks;
};

/**
 * sky1_set_mode_by_id - program one USB controller mode strap
 * @syscon: regmap for S5 syscon (from DT property cix,syscon-usb)
 * @id: controller slot ID (U3_TYPEC_DRD_ID .. U2_HOST3_ID)
 * @mode: MODE_STRAP_OTG, MODE_STRAP_HOST, or MODE_STRAP_DEVICE
 */
static int cdnsp_sky1_set_mode_by_id(struct regmap *syscon, int id, int mode)
{
	if (id < 0 || id >= SKY1_USB_S5_NUM)
		return -EINVAL;

	return regmap_update_bits(syscon,
				  strap_signals[id].offset,
				  GENMASK(strap_signals[id].bit + 1,
					  strap_signals[id].bit),
				  (unsigned int)mode << strap_signals[id].bit);
}

static int cdnsp_sky1_set_all_controllers_otg(struct regmap *syscon)
{
	int id, ret;

	for (id = 0; id < SKY1_USB_S5_NUM; id++) {
		ret = cdnsp_sky1_set_mode_by_id(syscon, id, MODE_STRAP_OTG);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct clk_bulk_data cdnsp_sky1_cdns_core_clks[] = {
	{ .id = "sof" },
	{ .id = "aclk" },
	{ .id = "lpm" },
	{ .id = "pclk" },
};

static int cdnsp_sky1_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cdnsp_sky1 *priv;
	struct cdns *cdns;
	struct cdns3_probe_data probe_data;
	struct resource *res;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->num_clks = ARRAY_SIZE(cdnsp_sky1_cdns_core_clks);
	priv->clks = devm_kmemdup(dev, cdnsp_sky1_cdns_core_clks,
				   sizeof(cdnsp_sky1_cdns_core_clks), GFP_KERNEL);
	if (!priv->clks)
		return -ENOMEM;

	ret = devm_clk_bulk_get(dev, priv->num_clks, priv->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks\n");

	ret = clk_bulk_prepare_enable(priv->num_clks, priv->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	priv->usb_syscon = syscon_regmap_lookup_by_phandle(dev->of_node,
							   "cix,syscon-usb");
	if (IS_ERR(priv->usb_syscon))
		return dev_err_probe(dev, PTR_ERR(priv->usb_syscon),
				     "failed to get cix,syscon-usb regmap\n");

	ret = cdnsp_sky1_set_all_controllers_otg(priv->usb_syscon);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to set USB controllers to OTG strap\n");

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "glue");
	if (!res)
		goto err_clk;

	priv->glue_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->glue_base)) {
		ret = PTR_ERR(priv->glue_base);
		goto err_clk;
	}

	/* Set ARCACHE and AWCACHE */
	writel(SKY1_USB_AXI_WR_CACHE_VALUE, priv->glue_base + AXI_SETTING_OFFSET);

	cdns = &priv->cdns;
	cdns->dev = dev;

	probe_data.cdns = cdns;
	probe_data.pdev = pdev;

	ret = cdns3_core_probe(&probe_data);
	if (ret)
		goto err_clk;

	return 0;

err_clk:
	clk_bulk_disable_unprepare(priv->num_clks, priv->clks);

	return ret;
}

static void cdnsp_sky1_remove(struct platform_device *pdev)
{
	struct cdns *cdns = platform_get_drvdata(pdev);
	struct cdnsp_sky1 *priv;

	if (!cdns)
		return;

	cdns3_core_remove(cdns);
	priv = container_of(cdns, struct cdnsp_sky1, cdns);
	clk_bulk_disable_unprepare(priv->num_clks, priv->clks);
}

#ifdef CONFIG_PM
static int cdnsp_sky1_runtime_suspend(struct device *dev)
{
	return cdns3_runtime_suspend(dev_get_drvdata(dev));
}

static int cdnsp_sky1_runtime_resume(struct device *dev)
{
	return cdns3_runtime_resume(dev_get_drvdata(dev));
}

#ifdef CONFIG_PM_SLEEP
static int cdnsp_sky1_suspend(struct device *dev)
{
	return cdns3_pm_suspend(dev_get_drvdata(dev));
}

static int cdnsp_sky1_resume(struct device *dev)
{
	return cdns3_pm_resume(dev_get_drvdata(dev));
}
#endif /* CONFIG_PM_SLEEP */
#endif /* CONFIG_PM */

static const struct dev_pm_ops cdnsp_sky1_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cdnsp_sky1_suspend, cdnsp_sky1_resume)
	SET_RUNTIME_PM_OPS(cdnsp_sky1_runtime_suspend,
			   cdnsp_sky1_runtime_resume, NULL)
};

static const struct of_device_id cdnsp_sky1_of_match[] = {
	{ .compatible = "cix,sky1-usb3" },
	{ }
};
MODULE_DEVICE_TABLE(of, cdnsp_sky1_of_match);

static struct platform_driver cdnsp_sky1_driver = {
	.probe		= cdnsp_sky1_probe,
	.remove		= cdnsp_sky1_remove,
	.driver		= {
		.name		= "cdnsp-sky1",
		.of_match_table	= of_match_ptr(cdnsp_sky1_of_match),
		.pm		= &cdnsp_sky1_pm_ops,
	},
};

module_platform_driver(cdnsp_sky1_driver);

MODULE_SOFTDEP("pre: cdns");
MODULE_ALIAS("platform:cdnsp-sky1");
MODULE_DESCRIPTION("CIX Sky1 Cadence USBSSP DRD glue driver");
MODULE_AUTHOR("Peter Chen <peter.chen@cixtech.com>");
MODULE_LICENSE("GPL");
