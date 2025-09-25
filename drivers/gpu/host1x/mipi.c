/*
 * Copyright (C) 2013 NVIDIA Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
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

int tegra_mipi_add_provider(struct device_node *np, const struct tegra_mipi_ops *ops)
{
	if (provider.np)
		return -EBUSY;

	provider.np = np;
	provider.ops = ops;

	return 0;
}
EXPORT_SYMBOL(tegra_mipi_add_provider);
