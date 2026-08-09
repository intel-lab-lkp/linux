// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Stefan Dösinger
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

enum zx297520v3_parent_type {
	ZX297520V3_INVALID = 0,
	ZX297520V3_TOPCRM,
	ZX297520V3_MATRIXCRM,
	ZX297520V3_LSPCRM,
};

static const struct mfd_cell zx297520v3_topcrm_cells[] = {
	{
		.name = "zx297520v3-topclk",
	},
	{
		.name = "zx297520v3-topreset",
	},
	{
		.name = "reboot",
		.of_compatible = "syscon-reboot",
	},
	{
		.name = "zx297520v3-usb-phy",
	},
};

static const struct mfd_cell zx297520v3_matrixcrm_cells[] = {
	{
		.name = "zx297520v3-matrixclk",
	},
	{
		.name = "zx297520v3-matrixreset",
	},
	/* A set of hwlock controllers is found here as well, but no driver is implemented yet */
};

static const struct mfd_cell zx297520v3_lspcrm_cells[] = {
	{
		.name = "zx297520v3-lspclk",
	},
	{
		.name = "zx297520v3-lspreset",
	},
};

static int zx297520v3_crm_probe(struct platform_device *pdev)
{
	enum zx297520v3_parent_type type;
	struct device *dev = &pdev->dev;
	const struct mfd_cell *cells;
	struct reset_control *rst;
	unsigned int num_cells;
	struct clk *pclk;

	type = (enum zx297520v3_parent_type)(kernel_ulong_t)of_device_get_match_data(dev);
	switch (type) {
	case ZX297520V3_TOPCRM:
		cells = zx297520v3_topcrm_cells;
		num_cells = ARRAY_SIZE(zx297520v3_topcrm_cells);
		break;

	case ZX297520V3_MATRIXCRM:
		cells = zx297520v3_matrixcrm_cells;
		num_cells = ARRAY_SIZE(zx297520v3_matrixcrm_cells);
		break;

	case ZX297520V3_LSPCRM:
		cells = zx297520v3_lspcrm_cells;
		num_cells = ARRAY_SIZE(zx297520v3_lspcrm_cells);

		pclk = devm_clk_get_enabled(dev, "pclk");
		if (IS_ERR(pclk))
			return dev_err_probe(dev, PTR_ERR(pclk), "Could not get pclk\n");

		rst = devm_reset_control_get_exclusive_deasserted(dev, NULL);
		if (IS_ERR(rst))
			return dev_err_probe(dev, PTR_ERR(rst), "Could not get reset\n");
		break;

	default:
		return -ENODEV;
	}

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, cells, num_cells, NULL, 0, NULL);
}

static const struct of_device_id of_match_zx297520v3_crm[] = {
	{ .compatible = "zte,zx297520v3-topcrm", .data = (void *)ZX297520V3_TOPCRM },
	{ .compatible = "zte,zx297520v3-matrixcrm", .data = (void *)ZX297520V3_MATRIXCRM },
	{ .compatible = "zte,zx297520v3-lspcrm", .data = (void *)ZX297520V3_LSPCRM },
	{ }
};
MODULE_DEVICE_TABLE(of, of_match_zx297520v3_crm);

static struct platform_driver zx297520v3_crm = {
	.probe = zx297520v3_crm_probe,
	.driver = {
		.name = "zx297520v3-crm",
		.of_match_table = of_match_zx297520v3_crm,
	},
};
module_platform_driver(zx297520v3_crm);

MODULE_AUTHOR("Stefan Dösinger <stefandoesinger@gmail.com>");
MODULE_DESCRIPTION("ZTE zx297520v3 CRM MFD host driver");
MODULE_LICENSE("GPL");
