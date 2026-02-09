// SPDX-License-Identifier: GPL-2.0

#include <linux/array_size.h>
#include <linux/of.h>
#include <linux/mfd/core.h>
#include <linux/mfd/syscon.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

static const struct mfd_cell sky1_system_control_devs[] = {
	MFD_CELL_NAME("cix,sky1-rst-fch"),
};

static const struct mfd_cell sky1_s5_system_control_devs[] = {
	MFD_CELL_NAME("cix,sky1-rst"),
};

static int sky1_system_control_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct mfd_cell *cell =
			(struct mfd_cell *)of_device_get_match_data(dev);

	return mfd_add_devices(dev, PLATFORM_DEVID_NONE, cell, 1, NULL, 0, NULL);
}

static const struct of_device_id sky1_system_control_of_match[] = {
	{ .compatible = "cix,sky1-system-control",
	  .data = sky1_system_control_devs},
	{ .compatible = "cix,sky1-s5-system-control",
	  .data = sky1_s5_system_control_devs},
	{},
};
MODULE_DEVICE_TABLE(of, sky1_system_control_of_match);

static struct platform_driver sky1_system_control_driver = {
	.driver = {
		.name = "sky1-system-control",
		.of_match_table = sky1_system_control_of_match,
	},
	.probe = sky1_system_control_probe,
};
module_platform_driver(sky1_system_control_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gary Yang <gary.yang@cixtech.com>");
MODULE_DESCRIPTION("Cix SoC system control driver");
