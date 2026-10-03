// SPDX-License-Identifier: GPL-2.0
/*
 * Altera Timeout Bridge Bus Driver
 *
 * Copyright (C) 2026 Bruker Corporation
 */

#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define ALTERA_TIMEOUT_BRIDGE_REG_TIMED_OUT_ADDRESS 0x08
#define ALTERA_TIMEOUT_BRIDGE_REG_IRQ_STATUS_RESET 0x00

struct altera_timeout_bridge_data {
	struct device *dev;
	struct resource *bridge_register_resource;
	void __iomem *bridge_registers;
	atomic_t irq_count;
	struct kernfs_node *irq_node;
};

static ssize_t timeout_irq_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return 0;
}

static ssize_t irq_count_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct altera_timeout_bridge_data *driver_data = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", atomic_read(&driver_data->irq_count));
}

static DEVICE_ATTR_RO(timeout_irq);
static DEVICE_ATTR_RO(irq_count);

static struct attribute *altera_timeout_bridge_sysfs_attrs[] = {
	&dev_attr_timeout_irq.attr,
	&dev_attr_irq_count.attr,
	NULL /* sentinel */
};

static const struct attribute_group altera_timeout_bridge_sysfs_group = {
	.attrs = altera_timeout_bridge_sysfs_attrs,
};

static u32
bridge_reg_read(struct altera_timeout_bridge_data *driver_data,
		u32 offset)
{
	u32 result;

	result = readl(driver_data->bridge_registers + offset);

	return result;
}

static void bridge_reg_write(
	struct altera_timeout_bridge_data *driver_data,
	int offset, u32 value)
{
	writel(value, driver_data->bridge_registers + offset);
}

static irqreturn_t altera_timeout_bridge_isr(int irq, void *data)
{
	struct altera_timeout_bridge_data *driver_data = data;
	u32 timed_out_address;
	resource_size_t effective_base_address;
	resource_size_t effective_timed_out_address;

	atomic_inc(&driver_data->irq_count);

	// Read and print the timed-out address (encoded in the lower nibble
	// 0x8..0xF according to AXI Timeout Bridge IP (CSR Interrupt Status Information)
	timed_out_address = bridge_reg_read(
		driver_data,
		ALTERA_TIMEOUT_BRIDGE_REG_TIMED_OUT_ADDRESS);
	effective_base_address = driver_data->bridge_register_resource->start;
	effective_timed_out_address = effective_base_address + timed_out_address;
	pr_err("%s: timeout at address 0x%llx\n",
		__func__, (unsigned long long)effective_timed_out_address);

	// Reset IRQ status in the bridge
	bridge_reg_write(
		driver_data,
		ALTERA_TIMEOUT_BRIDGE_REG_IRQ_STATUS_RESET,
		0x1);
	// notify the user space
	sysfs_notify_dirent(driver_data->irq_node);

	return IRQ_HANDLED;
}
static int altera_timeout_bridge_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_device_id *match;
	const struct of_dev_auxdata *lookup = dev_get_platdata(dev);
	struct device_node *np = dev->of_node;
	struct altera_timeout_bridge_data *driver_data;
	int ret = 0;
	int irq = -1;

	/*
	 * Allow user to use driver_override to bind this driver to a
	 * transparent bus device which has a different compatible string
	 * that's not listed in simple_pm_bus_of_match. We don't want to do any
	 * of the simple-pm-bus tasks for these devices, so return early.
	 */
	if (device_has_driver_override(&pdev->dev))
		return 0;

	match = of_match_device(dev->driver->of_match_table, dev);

	/* Use a fall back logic like in simple-pm-bus */
	if (match && match->data) {
		if (of_property_match_string(np, "compatible", match->compatible) == 0)
			return 0;
		else
			return -ENODEV;
	}

	driver_data =
		devm_kzalloc(&pdev->dev, sizeof(*driver_data), GFP_KERNEL);
	if (!driver_data) {
		dev_err(&pdev->dev,
			"%s: kzalloc failed: no memory for the driver's data, returning ENOMEM\n",
			__func__);
		return -ENOMEM;
	}

	driver_data->dev = dev;
	platform_set_drvdata(pdev, driver_data);

	driver_data->bridge_register_resource =
		platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!driver_data->bridge_register_resource) {
		dev_err(dev,
			"%s: missing required reg binding\n",
			__func__);
		return -ENOENT;
	}
	driver_data->bridge_registers =
		devm_ioremap_resource(dev,
				     driver_data->bridge_register_resource);
	if (IS_ERR(driver_data->bridge_registers)) {
		dev_err(dev,
			"%s: ioremap_resource failed for bridge-status MMIO, returning PTR_ERR(%ld)\n",
			__func__, PTR_ERR(driver_data->bridge_registers));
		return PTR_ERR(driver_data->bridge_registers);
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev,
			"%s: platform_get_irq failed: %d\n", __func__, irq);
		return irq;
	}

	ret = devm_request_irq(dev, irq,
			       altera_timeout_bridge_isr,
			       0,
			       dev_name(dev),
			       driver_data);
	if (ret) {
		dev_err(dev,
			"%s: devm_request_irq failed for IRQ %d: %d\n",
			__func__, irq, ret);
		return ret;
	}

	ret = devm_device_add_group(dev, &altera_timeout_bridge_sysfs_group);
	if (ret)
		return ret;

	driver_data->irq_node = sysfs_get_dirent(dev->kobj.sd, "timeout_irq");
	if (!driver_data->irq_node)
		return -ENOENT;

	ret = devm_add_action_or_reset(dev, (void (*)(void *))sysfs_put, driver_data->irq_node);
	if (ret)
		return ret;

	if (np)
		of_platform_populate(np, NULL, lookup, &pdev->dev);

	return 0;
}

#define ONLY_BUS	((void *) 1) /* Match if the device is only a bus. */

static const struct of_device_id altera_timeout_bridge_of_match[] = {
	{ .compatible = "altera-timeout-bridge", },
	{ .compatible = "simple-bus",	.data = ONLY_BUS },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, altera_timeout_bridge_of_match);

static struct platform_driver altera_timeout_bridge_driver = {
	.probe = altera_timeout_bridge_probe,
	.driver = {
		.name = "altera-timeout-bridge",
		.of_match_table = altera_timeout_bridge_of_match,
	},
};

module_platform_driver(altera_timeout_bridge_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Avalon Timeout Bridge Bus");
MODULE_AUTHOR("Vyacheslav Yurkov (V.Yurkov.EXT@bruker.com)");

