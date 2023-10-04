// SPDX-License-Identifier: GPL-2.0
/*
 * NVMEM layout bus handling
 *
 * Copyright (C) 2023 Bootlin
 * Author: Miquel Raynal <miquel.raynal@bootlin.com
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/nvmem-consumer.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>

#include "internals.h"

#if CONFIG_OF
static int nvmem_layout_bus_match(struct device *dev, struct device_driver *drv)
{
	return of_driver_match_device(dev, drv);
}

static struct bus_type nvmem_layout_bus_type = {
	.name		= "nvmem-layouts",
	.match		= nvmem_layout_bus_match,
};

static struct device nvmem_layout_bus = {
	.init_name	= "nvmem-layouts",
};

int __nvmem_layout_driver_register(struct nvmem_layout_driver *drv,
				   struct module *owner)
{
	drv->driver.owner = owner;
	drv->driver.bus = &nvmem_layout_bus_type;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(__nvmem_layout_driver_register);

void nvmem_layout_driver_unregister(struct nvmem_layout_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(nvmem_layout_driver_unregister);

static void nvmem_layout_device_release(struct device *dev)
{
	of_node_put(dev->of_node);
	kfree(dev);
}

static struct device *of_nvmem_layout_create_device(struct device_node *np)
{
	struct device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return NULL;

	device_initialize(dev);
	dev->parent = &nvmem_layout_bus;
	dev->bus = &nvmem_layout_bus_type;
	dev->release = nvmem_layout_device_release;
	dev->coherent_dma_mask = DMA_BIT_MASK(32);
	dev->dma_mask = &dev->coherent_dma_mask;
	device_set_node(dev, of_fwnode_handle(of_node_get(np)));
	of_device_make_bus_id(dev);
	of_msi_configure(dev, dev->of_node);

	if (device_add(dev)) {
		put_device(dev);
		return NULL;
	}

	return dev;
}

static const struct of_device_id of_nvmem_layout_skip_table[] = {
	{ .compatible = "fixed-layout", },
	{}
};

static int of_nvmem_layout_bus_populate(struct device_node *layout_dn)
{
	/* Make sure it has a compatible property */
	if (!of_get_property(layout_dn, "compatible", NULL)) {
		pr_debug("%s() - skipping %pOF, no compatible prop\n",
			 __func__, layout_dn);
		return 0;
	}

	/* Fixed layouts are parsed manually somewhere else for now */
	if (of_match_node(of_nvmem_layout_skip_table, layout_dn)) {
		pr_debug("%s() - skipping %pOF node\n", __func__, layout_dn);
		return 0;
	}

	if (of_node_check_flag(layout_dn, OF_POPULATED_BUS)) {
		pr_debug("%s() - skipping %pOF, already populated\n",
			 __func__, layout_dn);
		return 0;
	}

	/* NVMEM layout buses expect only a single device representing the layout */
	of_nvmem_layout_create_device(layout_dn);
	of_node_set_flag(layout_dn, OF_POPULATED_BUS);

	return 0;
}

struct device_node *of_nvmem_layout_get_container(struct nvmem_device *nvmem)
{
	return of_get_child_by_name(nvmem->dev.of_node, "nvmem-layout");
}
EXPORT_SYMBOL_GPL(of_nvmem_layout_get_container);

int nvmem_populate_layout(struct nvmem_device *nvmem)
{
	struct device_node *nvmem_dn, *layout_dn;
	int ret;

	nvmem_dn = of_node_get(nvmem->dev.of_node);
	if (!nvmem_dn)
		return 0;

	layout_dn = of_nvmem_layout_get_container(nvmem);
	if (!layout_dn) {
		of_node_put(nvmem_dn);
		return 0;
	}

	device_links_supplier_sync_state_pause();
	ret = of_nvmem_layout_bus_populate(layout_dn);
	device_links_supplier_sync_state_resume();

	of_node_set_flag(nvmem_dn, OF_POPULATED_BUS);

	of_node_put(layout_dn);
	of_node_put(nvmem_dn);
	return ret;
}

int nvmem_layout_bus_register(void)
{
	int ret;

	ret = device_register(&nvmem_layout_bus);
	if (ret) {
		put_device(&nvmem_layout_bus);
		return ret;
	}

	ret = bus_register(&nvmem_layout_bus_type);
	if (ret) {
		device_unregister(&nvmem_layout_bus);
		return ret;
	}

	return 0;
}

void nvmem_layout_bus_unregister(void)
{
	bus_unregister(&nvmem_layout_bus_type);
	device_unregister(&nvmem_layout_bus);
}
#endif
