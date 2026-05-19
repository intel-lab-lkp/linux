// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/device.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/qda_compute_bus.h>
#include <linux/slab.h>

static int qda_cb_bus_dma_configure(struct device *dev)
{
	return of_dma_configure(dev, dev->of_node, true);
}

const struct bus_type qda_cb_bus_type = {
	.name = "qda-compute-cb",
	.dma_configure = qda_cb_bus_dma_configure,
};
EXPORT_SYMBOL_GPL(qda_cb_bus_type);

static void release_qda_cb_device(struct device *dev)
{
	of_node_put(dev->of_node);
	kfree(dev);
}

struct device *create_qda_cb_device(struct device *parent_device, const char *name,
				    u64 dma_mask, struct device_node *of_node)
{
	struct device *dev;
	int ret;

	dev = kzalloc_obj(*dev);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	dev->release = release_qda_cb_device;
	dev->bus = &qda_cb_bus_type;
	dev->parent = parent_device;
	dev->coherent_dma_mask = dma_mask;
	dev->dma_mask = &dev->coherent_dma_mask;
	dev->of_node = of_node_get(of_node);

	dev_set_name(dev, "%s", name);

	ret = device_register(dev);
	if (ret) {
		put_device(dev);
		return ERR_PTR(ret);
	}

	return dev;
}
EXPORT_SYMBOL_GPL(create_qda_cb_device);

static int __init qda_cb_bus_init(void)
{
	int err;

	err = bus_register(&qda_cb_bus_type);
	if (err < 0) {
		pr_err("qda-compute-cb bus registration failed: %d\n", err);
		return err;
	}
	return 0;
}

postcore_initcall(qda_cb_bus_init);
