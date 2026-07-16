// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Stefan Dösinger
 */

#include <linux/platform_device.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/of.h>

struct zx297520v3_crm_data {
	const struct mfd_cell *cells;
	unsigned int num_cells;
};

static const struct mfd_cell zx297520v3_topcrm_devs[] = {
	{
		.name = "zx297520v3-topclk",
	},
	{
		.name = "zx297520v3-topreset",
	},
	{
		.name = "syscon-reboot",
		.of_compatible = "syscon-reboot",
	},
	{
		.name = "zx297520v3-usb-phy",
		.of_compatible = "zte,zx297520v3-usb-phy",
	},
};

static const struct zx297520v3_crm_data zx297520v3_topcrm_data = {
	.cells = zx297520v3_topcrm_devs,
	.num_cells = ARRAY_SIZE(zx297520v3_topcrm_devs),
};

static const struct mfd_cell zx297520v3_matrixcrm_devs[] = {
	{
		.name = "zx297520v3-matrixclk",
	},
	{
		.name = "zx297520v3-matrixreset",
	},
	/* A set of hwlock controllers is found here as well, but no driver is implemented yet */
};

static const struct zx297520v3_crm_data zx297520v3_matrixcrm_data = {
	.cells = zx297520v3_matrixcrm_devs,
	.num_cells = ARRAY_SIZE(zx297520v3_matrixcrm_devs),
};

static const struct mfd_cell zx297520v3_lspcrm_devs[] = {
	{
		.name = "zx297520v3-lspclk",
	},
	{
		.name = "zx297520v3-lspreset",
	},
};

static const struct zx297520v3_crm_data zx297520v3_lspcrm_data = {
	.cells = zx297520v3_lspcrm_devs,
	.num_cells = ARRAY_SIZE(zx297520v3_lspcrm_devs),
};

static int zx297520v3_crm_probe(struct platform_device *pdev)
{
	const struct zx297520v3_crm_data *data;

	data = of_device_get_match_data(&pdev->dev);
	if (!data)
		return -ENODEV;

	return devm_mfd_add_devices(&pdev->dev, PLATFORM_DEVID_NONE, data->cells,
				    data->num_cells, NULL, 0, NULL);
}

static const struct of_device_id of_match_zx297520v3_crm[] = {
	{ .compatible = "zte,zx297520v3-topcrm", .data = &zx297520v3_topcrm_data },
	{ .compatible = "zte,zx297520v3-matrixcrm", .data = &zx297520v3_matrixcrm_data },
	{ .compatible = "zte,zx297520v3-lspcrm", .data = &zx297520v3_lspcrm_data },
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
