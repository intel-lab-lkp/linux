// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/dma_context_bus.h>
#include <linux/of_device.h>

static atomic_t dma_context_bus_device_id = ATOMIC_INIT(0);

static int dma_context_bus_device_configure(struct device *dev)
{
	const u32 *iommu_fid = dev_get_drvdata(dev);
	struct device_node *of_node = dev->of_node;

	if (!of_node)
		of_node = dev->parent->of_node;

	return of_dma_configure_id(dev, of_node, true, iommu_fid);
}

const struct bus_type dma_context_bus_type = {
	.name = "dma-context-bus",
	.dma_configure = dma_context_bus_device_configure,
};
EXPORT_SYMBOL_GPL(dma_context_bus_type);

static void release_dma_context_bus_device(struct device *dev)
{
	kfree(dev);
}

struct device *create_dma_context_bus_device(struct device *parent_device,
					     struct device_node *of_node,
					     u64 dma_mask, const u32 *iommu_fid)
{
	struct device *dev;
	int dev_id, ret;

	dev = kzalloc_obj(*dev);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	dev->release = release_dma_context_bus_device;
	dev->bus = &dma_context_bus_type;
	dev->parent = parent_device;
	dev->coherent_dma_mask = dma_mask;
	dev->dma_mask = &dev->coherent_dma_mask;
	dev->of_node = of_node;

	dev_id = atomic_inc_return(&dma_context_bus_device_id);
	dev_set_name(dev, "dma-context-bus-%d", dev_id);
	dev_set_drvdata(dev, (void *)iommu_fid);

	ret = device_register(dev);
	if (ret) {
		put_device(dev);
		return ERR_PTR(ret);
	}

	return dev;
}
EXPORT_SYMBOL_GPL(create_dma_context_bus_device);

static int __init dma_context_bus_init(void)
{
	int err;

	err = bus_register(&dma_context_bus_type);
	if (err) {
		pr_err("dma-context-bus registration failed: %d\n", err);
		return err;
	}

	return 0;
}
postcore_initcall(dma_context_bus_init);
