// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 Huawei. All rights reserved. */

#include <linux/device.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <cxlmem.h>
#include <hmu.h>
#include <cxl.h>
#include "core.h"

static void cxl_hmu_release(struct device *dev)
{
	struct cxl_hmu *hmu = to_cxl_hmu(dev);

	kfree(hmu);
}

const struct device_type cxl_hmu_type = {
	.name = "cxl_hmu",
	.release = cxl_hmu_release,
};

static void remove_dev(void *dev)
{
	device_unregister(dev);
}

int devm_cxl_hmu_add(struct device *parent, struct cxl_hmu_regs *regs,
		     int assoc_id, int index)
{
	struct cxl_hmu *hmu;
	struct device *dev;
	int rc;

	hmu = kzalloc(sizeof(*hmu), GFP_KERNEL);
	if (!hmu)
		return -ENOMEM;

	hmu->assoc_id = assoc_id;
	hmu->index = index;
	hmu->base = regs->hmu;
	dev = &hmu->dev;
	device_initialize(dev);
	device_set_pm_not_required(dev);
	dev->parent = parent;
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_hmu_type;
	rc = dev_set_name(dev, "hmu_mem%d.%d", assoc_id, index);
	if (rc)
		goto err;

	rc = device_add(dev);
	if (rc)
		goto err;

	return devm_add_action_or_reset(parent, remove_dev, dev);

err:
	put_device(&hmu->dev);
	return rc;
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_hmu_add, CXL);

