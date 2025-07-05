// SPDX-License-Identifier: GPL-2.0-only
/*
 * dwc3-generic-plat.c - DesignWare USB3 generic platform driver
 *
 * Copyright (C) 2025 Ze Huang <huang.ze@linux.dev>
 *
 * Inspired by dwc3-qcom.c and dwc3-of-simple.c
 */

#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include "glue.h"

struct dwc3_generic {
	struct device		*dev;
	struct dwc3		dwc;
	struct clk_bulk_data	*clks;
	int			num_clocks;
	struct reset_control	*resets;
};

static void dwc3_generic_reset_control_assert(void *data)
{
	reset_control_assert(data);
}

static void dwc3_generic_clk_bulk_disable_unprepare(void *data)
{
	struct dwc3_generic *dwc3 = data;

	clk_bulk_disable_unprepare(dwc3->num_clocks, dwc3->clks);
}

static int dwc3_generic_probe(struct platform_device *pdev)
{
	struct dwc3_probe_data probe_data = {};
	struct device *dev = &pdev->dev;
	struct dwc3_generic *dwc3;
	struct resource *res;
	int ret;

	dwc3 = devm_kzalloc(dev, sizeof(*dwc3), GFP_KERNEL);
	if (!dwc3)
		return -ENOMEM;

	dwc3->dev = dev;

	platform_set_drvdata(pdev, dwc3);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "missing memory resource\n");
		return -ENODEV;
	}

	dwc3->resets = devm_reset_control_array_get_optional_exclusive(dev);
	if (IS_ERR(dwc3->resets))
		return dev_err_probe(dev, PTR_ERR(dwc3->resets), "failed to get resets\n");

	ret = reset_control_assert(dwc3->resets);
	if (ret)
		return dev_err_probe(dev, ret, "failed to assert resets\n");

	ret = devm_add_action_or_reset(dev, dwc3_generic_reset_control_assert, dwc3->resets);
	if (ret)
		return ret;

	usleep_range(10, 1000);

	ret = reset_control_deassert(dwc3->resets);
	if (ret)
		return dev_err_probe(dev, ret, "failed to deassert resets\n");

	ret = devm_clk_bulk_get_all(dwc3->dev, &dwc3->clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get clocks\n");

	dwc3->num_clocks = ret;

	ret = clk_bulk_prepare_enable(dwc3->num_clocks, dwc3->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	ret = devm_add_action_or_reset(dev, dwc3_generic_clk_bulk_disable_unprepare, dwc3);
	if (ret)
		return ret;

	dwc3->dwc.dev = dev;
	probe_data.dwc = &dwc3->dwc;
	probe_data.res = res;
	probe_data.ignore_clocks_and_resets = true;
	ret = dwc3_core_probe(&probe_data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register DWC3 Core\n");

	return 0;
}

static void dwc3_generic_remove(struct platform_device *pdev)
{
	struct dwc3_generic *dwc3 = platform_get_drvdata(pdev);

	dwc3_core_remove(&dwc3->dwc);
}

static int dwc3_generic_suspend(struct device *dev)
{
	struct dwc3_generic *dwc3 = dev_get_drvdata(dev);
	int ret;

	ret = dwc3_pm_suspend(&dwc3->dwc);
	if (ret)
		return ret;

	clk_bulk_disable_unprepare(dwc3->num_clocks, dwc3->clks);

	return 0;
}

static int dwc3_generic_resume(struct device *dev)
{
	struct dwc3_generic *dwc3 = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(dwc3->num_clocks, dwc3->clks);
	if (ret)
		return ret;

	ret = dwc3_pm_resume(&dwc3->dwc);
	if (ret)
		return ret;

	return 0;
}

static int dwc3_generic_runtime_suspend(struct device *dev)
{
	struct dwc3_generic *dwc3 = dev_get_drvdata(dev);

	return dwc3_runtime_suspend(&dwc3->dwc);
}

static int dwc3_generic_runtime_resume(struct device *dev)
{
	struct dwc3_generic *dwc3 = dev_get_drvdata(dev);

	return dwc3_runtime_resume(&dwc3->dwc);
}

static int dwc3_generic_runtime_idle(struct device *dev)
{
	struct dwc3_generic *dwc3 = dev_get_drvdata(dev);

	return dwc3_runtime_idle(&dwc3->dwc);
}

static const struct dev_pm_ops dwc3_generic_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dwc3_generic_suspend, dwc3_generic_resume)
	SET_RUNTIME_PM_OPS(dwc3_generic_runtime_suspend, dwc3_generic_runtime_resume,
			   dwc3_generic_runtime_idle)
};

static const struct of_device_id dwc3_generic_of_match[] = {
	{ .compatible = "spacemit,k1-dwc3", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, dwc3_generic_of_match);

static struct platform_driver dwc3_generic_driver = {
	.probe		= dwc3_generic_probe,
	.remove		= dwc3_generic_remove,
	.driver		= {
		.name	= "dwc3-generic-plat",
		.of_match_table = dwc3_generic_of_match,
		.pm	= &dwc3_generic_dev_pm_ops,
	},
};
module_platform_driver(dwc3_generic_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DesignWare USB3 generic platform driver");
