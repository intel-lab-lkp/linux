// SPDX-License-Identifier: GPL-2.0
/*
 * VBATTB driver
 *
 * Copyright (C) 2024 Renesas Electronics Corp.
 */

#include <linux/mod_devicetable.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

static int vbattb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *rstc;
	int ret;

	rstc = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(rstc))
		return PTR_ERR(rstc);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	ret = reset_control_deassert(rstc);
	if (ret)
		goto rpm_put;

	platform_set_drvdata(pdev, rstc);

	ret = devm_of_platform_populate(dev);
	if (ret)
		goto reset_assert;

	return 0;

reset_assert:
	reset_control_assert(rstc);
rpm_put:
	pm_runtime_put(dev);
	return ret;
}

static void vbattb_remove(struct platform_device *pdev)
{
	struct reset_control *rstc = platform_get_drvdata(pdev);

	reset_control_assert(rstc);
	pm_runtime_put(&pdev->dev);
}

static const struct of_device_id vbattb_match[] = {
	{ .compatible = "renesas,r9a08g045-vbattb" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, vbattb_match);

static struct platform_driver vbattb_driver = {
	.probe = vbattb_probe,
	.remove_new = vbattb_remove,
	.driver = {
		.name = "renesas-vbattb",
		.of_match_table = vbattb_match,
	},
};
module_platform_driver(vbattb_driver);

MODULE_ALIAS("platform:renesas-vbattb");
MODULE_AUTHOR("Claudiu Beznea <claudiu.beznea.uj@bp.renesas.com>");
MODULE_DESCRIPTION("Renesas VBATTB driver");
MODULE_LICENSE("GPL");
