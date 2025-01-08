// SPDX-License-Identifier: GPL-2.0
/*
 * Simple Bus Driver
 */

#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

static struct platform_driver simple_bus_driver;

static int has_specific_simple_bus_drv(struct device_driver *drv, void *dev)
{
	/* Skip if it's this simple bus driver */
	if (drv == &simple_bus_driver.driver)
		return 0;

	if (of_driver_match_device(dev, drv)) {
		dev_dbg(dev, "Allowing '%s' to probe more specifically\n", drv->name);
		return 1;
	}

	return 0;
}

static int simple_bus_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_dev_auxdata *lookup = dev_get_platdata(dev);
	struct device_node *np = dev->of_node;

	/*
	 * If any other driver wants the device, leave the device to the other
	 * driver. Only check drivers that come after this driver so that if an
	 * earlier driver failed to probe we don't populate any devices, and
	 * only check if there's a more specific compatible.
	 */
	if (of_property_match_string(np, "compatible", "simple-bus") != 0 &&
	    bus_for_each_drv(&platform_bus_type, &simple_bus_driver.driver, dev,
			     has_specific_simple_bus_drv))
		return -ENODEV;

	if (np)
		of_platform_populate(np, NULL, lookup, dev);

	return 0;
}

static const struct of_device_id simple_bus_of_match[] = {
	{ .compatible = "simple-bus", },
	{ }
};
MODULE_DEVICE_TABLE(of, simple_bus_of_match);

static struct platform_driver simple_bus_driver = {
	.probe = simple_bus_probe,
	.driver = {
		.name = "simple-bus",
		.of_match_table = simple_bus_of_match,
	},
};

static int __init simple_bus_driver_init(void)
{
	return platform_driver_register(&simple_bus_driver);
}
arch_initcall(simple_bus_driver_init);

static void __exit simple_bus_driver_exit(void)
{
	platform_driver_unregister(&simple_bus_driver);
}
module_exit(simple_bus_driver_exit);

MODULE_DESCRIPTION("Simple Bus Driver");
MODULE_LICENSE("GPL");
