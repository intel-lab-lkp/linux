// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 NVIDIA Corporation
 * Copyright (C) 2025 Svyatoslav Ryhel <clamor95@gmail.com>
 */

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/tegra-mipi-cal.h>

/* only need to support one provider */
static struct {
	struct device_node *np;
	const struct tegra_mipi_ops *ops;
} provider;

int tegra_mipi_enable(struct tegra_mipi_device *device)
{
	if (device->ops->enable)
		return device->ops->enable(device);

	return 0;
}
EXPORT_SYMBOL(tegra_mipi_enable);

int tegra_mipi_disable(struct tegra_mipi_device *device)
{
	if (device->ops->disable)
		return device->ops->disable(device);

	return 0;
}
EXPORT_SYMBOL(tegra_mipi_disable);

int tegra_mipi_start_calibration(struct tegra_mipi_device *device)
{
	if (device->ops->start_calibration)
		return device->ops->start_calibration(device);

	return 0;
}
EXPORT_SYMBOL(tegra_mipi_start_calibration);

int tegra_mipi_finish_calibration(struct tegra_mipi_device *device)
{
	if (device->ops->finish_calibration)
		return device->ops->finish_calibration(device);

	return 0;
}
EXPORT_SYMBOL(tegra_mipi_finish_calibration);

struct tegra_mipi_device *tegra_mipi_request(struct device *device,
					     struct device_node *np)
{
	struct tegra_mipi_device *mipidev;
	struct of_phandle_args args;
	int err;

	err = of_parse_phandle_with_args(np, "nvidia,mipi-calibrate",
					 "#nvidia,mipi-calibrate-cells", 0,
					 &args);
	if (err < 0)
		return ERR_PTR(err);

	if (provider.np != args.np)
		return ERR_PTR(-ENODEV);

	mipidev = kzalloc(sizeof(*mipidev), GFP_KERNEL);
	if (!mipidev) {
		err = -ENOMEM;
		goto out;
	}

	mipidev->pdev = of_find_device_by_node(args.np);
	if (!mipidev->pdev) {
		err = -ENODEV;
		goto free;
	}

	of_node_put(args.np);

	mipidev->ops = provider.ops;
	mipidev->pads = args.args[0];

	return mipidev;

free:
	kfree(mipidev);
out:
	of_node_put(args.np);
	return ERR_PTR(err);
}
EXPORT_SYMBOL(tegra_mipi_request);

void tegra_mipi_free(struct tegra_mipi_device *mipidev)
{
	platform_device_put(mipidev->pdev);
	kfree(mipidev);
}
EXPORT_SYMBOL(tegra_mipi_free);

static void tegra_mipi_remove_provider(void *data)
{
	provider.np = NULL;
	provider.ops = NULL;
}

int devm_tegra_mipi_add_provider(struct device *device, struct device_node *np,
				 const struct tegra_mipi_ops *ops)
{
	if (provider.np)
		return -EBUSY;

	provider.np = np;
	provider.ops = ops;

	return devm_add_action_or_reset(device, tegra_mipi_remove_provider, NULL);
}
EXPORT_SYMBOL(devm_tegra_mipi_add_provider);
