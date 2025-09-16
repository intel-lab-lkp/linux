// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, Dang Huynh <dang.huynh@mainlining.org>
 *
 * Based on drivers/power/reset/msm-poweroff.c:
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/pm.h>
#include <linux/mfd/syscon.h>

static void __iomem *rda_md_sysctrl;

static int do_rda_reboot(struct sys_off_data *data)
{
	/* unprotect md registers */
	writel(0x00A50001, rda_md_sysctrl);

	/* reset all */
	writel(0x80000000, rda_md_sysctrl + 4);

	return NOTIFY_DONE;
}

static int rda_reboot_probe(struct platform_device *pdev)
{
	rda_md_sysctrl = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rda_md_sysctrl))
		return PTR_ERR(rda_md_sysctrl);

	devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_RESTART,
				      128, do_rda_reboot, NULL);

	return 0;
}

static const struct of_device_id of_rda_reboot_match[] = {
	{ .compatible = "rda,md-reset", },
	{},
};
MODULE_DEVICE_TABLE(of, of_rda_reboot_match);

static struct platform_driver rda_reboot_driver = {
	.probe = rda_reboot_probe,
	.driver = {
		.name = "rda-reboot",
		.of_match_table = of_match_ptr(of_rda_reboot_match),
	},
};
builtin_platform_driver(rda_reboot_driver);
