// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/device.h>
#include <linux/of_device.h>

#include "iris_platform_common.h"

static int iris_vpu_bus_dma_configure(struct device *dev)
{
	const u32 *f_id = dev_get_drvdata(dev);

	if (!f_id)
		return -ENODEV;

	return of_dma_configure_id(dev, dev->parent->of_node, true, f_id);
}

const struct bus_type iris_vpu_bus_type = {
	.name = "iris-vpu-bus",
	.dma_configure = iris_vpu_bus_dma_configure,
};
EXPORT_SYMBOL_GPL(iris_vpu_bus_type);

static int __init iris_vpu_bus_init(void)
{
	return bus_register(&iris_vpu_bus_type);
}

postcore_initcall(iris_vpu_bus_init);
