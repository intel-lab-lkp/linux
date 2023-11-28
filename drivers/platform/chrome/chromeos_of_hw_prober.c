// SPDX-License-Identifier: GPL-2.0-only
/*
 * ChromeOS Device Tree Hardware Prober
 *
 * Copyright (c) 2023 Google LLC
 */

#include <linux/array_size.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define DRV_NAME	"chromeos_of_hw_prober"

/**
 * struct hw_prober_entry - Holds an entry for the hardware prober
 *
 * @compatible:	compatible string to match against the machine
 * @prober:	prober function to call when machine matches
 * @data:	extra data for the prober function
 */
struct hw_prober_entry {
	const char *compatible;
	int (*prober)(struct device *dev, const void *data);
	const void *data;
};

static int chromeos_i2c_component_prober(struct device *dev, const void *data)
{
	const char *type = data;

	return i2c_of_probe_component(dev, type);
}

static const struct hw_prober_entry hw_prober_platforms[] = {
	{ .compatible = "google,hana", .prober = chromeos_i2c_component_prober, .data = "touchscreen" },
	{ .compatible = "google,hana", .prober = chromeos_i2c_component_prober, .data = "trackpad" },
};

static int chromeos_of_hw_prober_probe(struct platform_device *pdev)
{
	for (size_t i = 0; i < ARRAY_SIZE(hw_prober_platforms); i++)
		if (of_machine_is_compatible(hw_prober_platforms[i].compatible)) {
			int ret;

			ret = hw_prober_platforms[i].prober(&pdev->dev,
							    hw_prober_platforms[i].data);
			if (ret)
				return ret;
		}

	return 0;
}

static struct platform_driver chromeos_of_hw_prober_driver = {
	.probe	= chromeos_of_hw_prober_probe,
	.driver	= {
		.name = DRV_NAME,
	},
};

static int __init chromeos_of_hw_prober_driver_init(void)
{
	struct platform_device *pdev;
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(hw_prober_platforms); i++)
		if (of_machine_is_compatible(hw_prober_platforms[i].compatible))
			break;
	if (i == ARRAY_SIZE(hw_prober_platforms))
		return 0;

	ret = platform_driver_register(&chromeos_of_hw_prober_driver);
	if (ret)
		return ret;

	pdev = platform_device_register_simple(DRV_NAME, PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(pdev))
		goto err;

	return 0;

err:
	platform_driver_unregister(&chromeos_of_hw_prober_driver);

	return PTR_ERR(pdev);
}
device_initcall(chromeos_of_hw_prober_driver_init);
