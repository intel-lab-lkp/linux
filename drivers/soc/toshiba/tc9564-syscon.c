// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2026 by RISCstar Solutions Corporation.  All rights reserved.
 */

#include <linux/device.h>
#include <linux/device-id/of.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>

static const struct mfd_cell tc9564_config_syscon_cells[] = {
	MFD_CELL_NAME("toshiba-tc9564-clock"),
	MFD_CELL_NAME("toshiba-tc9564-reset"),
};

static int tc9564_config_syscon_probe(struct platform_device *pdev)
{
	return devm_mfd_add_devices(&pdev->dev, PLATFORM_DEVID_AUTO,
				    tc9564_config_syscon_cells,
				    ARRAY_SIZE(tc9564_config_syscon_cells),
				    NULL, 0, NULL);
}

static const struct of_device_id tc9564_config_syscon_match[] = {
	{ .compatible = "toshiba,tc9564-config-syscon", },
	{ }
};
MODULE_DEVICE_TABLE(of, tc9564_config_syscon_match);

static struct platform_driver tc9564_config_syscon_driver = {
	.driver = {
		.name		= KBUILD_MODNAME,
		.of_match_table	= tc9564_config_syscon_match,
	},
	.probe = tc9564_config_syscon_probe,
};
module_platform_driver(tc9564_config_syscon_driver);

MODULE_DESCRIPTION("Toshiba TC9564 config syscon driver");
MODULE_LICENSE("GPL");
