// SPDX-License-Identifier: GPL-2.0-only
/*
 * ChromeOS Device Tree Hardware Prober
 *
 * Copyright (c) 2024 Google LLC
 */

#include <linux/array_size.h>
#include <linux/errno.h>
#include <linux/i2c-of-prober.h>
#include <linux/module.h>
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

struct chromeos_i2c_probe_data {
	const struct i2c_of_probe_cfg *cfg;
	const struct i2c_of_probe_simple_opts *opts;
};

static int chromeos_i2c_component_prober(struct device *dev, const void *_data)
{
	const struct chromeos_i2c_probe_data *data = _data;
	struct i2c_of_probe_simple_ctx ctx = {
		.opts = data->opts
	};

	return i2c_of_probe_component(dev, data->cfg, &ctx);
}

static const struct chromeos_i2c_probe_data chromeos_i2c_probe_dumb_touchscreen = {
	.cfg = &(const struct i2c_of_probe_cfg) {
		.type = "touchscreen"
	}
};

static const struct chromeos_i2c_probe_data chromeos_i2c_probe_dumb_trackpad = {
	.cfg = &(const struct i2c_of_probe_cfg) {
		.type = "trackpad"
	}
};

static const struct hw_prober_entry hw_prober_platforms[] = {
	{ .compatible = "google,hana", .prober = chromeos_i2c_component_prober, .data = &chromeos_i2c_probe_dumb_touchscreen },
	{ .compatible = "google,hana", .prober = chromeos_i2c_component_prober, .data = &chromeos_i2c_probe_dumb_trackpad },
};

static int chromeos_of_hw_prober_probe(struct platform_device *pdev)
{
	for (size_t i = 0; i < ARRAY_SIZE(hw_prober_platforms); i++) {
		int ret;

		if (!of_machine_is_compatible(hw_prober_platforms[i].compatible))
			continue;

		ret = hw_prober_platforms[i].prober(&pdev->dev, hw_prober_platforms[i].data);
		/* Ignore unrecoverable errors and keep going through other probers */
		if (ret == -EPROBE_DEFER)
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

static struct platform_device *chromeos_of_hw_prober_pdev;

static int chromeos_of_hw_prober_driver_init(void)
{
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(hw_prober_platforms); i++)
		if (of_machine_is_compatible(hw_prober_platforms[i].compatible))
			break;
	if (i == ARRAY_SIZE(hw_prober_platforms))
		return -ENODEV;

	ret = platform_driver_register(&chromeos_of_hw_prober_driver);
	if (ret)
		return ret;

	chromeos_of_hw_prober_pdev =
			platform_device_register_simple(DRV_NAME, PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(chromeos_of_hw_prober_pdev))
		goto err;

	return 0;

err:
	platform_driver_unregister(&chromeos_of_hw_prober_driver);

	return PTR_ERR(chromeos_of_hw_prober_pdev);
}
module_init(chromeos_of_hw_prober_driver_init);

static void chromeos_of_hw_prober_driver_exit(void)
{
	platform_device_unregister(chromeos_of_hw_prober_pdev);
	platform_driver_unregister(&chromeos_of_hw_prober_driver);
}
module_exit(chromeos_of_hw_prober_driver_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ChromeOS device tree hardware prober");
MODULE_IMPORT_NS(I2C_OF_PROBER);
