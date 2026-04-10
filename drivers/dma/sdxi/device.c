// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI hardware device driver
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#include <linux/device.h>
#include <linux/slab.h>

#include "sdxi.h"

int sdxi_register(struct device *dev, const struct sdxi_bus_ops *ops)
{
	struct sdxi_dev *sdxi;

	sdxi = devm_kzalloc(dev, sizeof(*sdxi), GFP_KERNEL);
	if (!sdxi)
		return -ENOMEM;

	sdxi->dev = dev;
	sdxi->bus_ops = ops;
	dev_set_drvdata(dev, sdxi);

	return sdxi->bus_ops->init(sdxi);
}
