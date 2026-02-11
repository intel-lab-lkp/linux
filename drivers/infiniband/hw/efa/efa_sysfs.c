// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Copyright 2018-2026 Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <linux/device.h>
#include <linux/sysfs.h>

#include "efa_sysfs.h"

static ssize_t ah_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct efa_dev *efa_dev = pci_get_drvdata(to_pci_dev(dev));

	return sysfs_emit(buf, "%lld\n", atomic64_read(&efa_dev->ah_count));
}

static DEVICE_ATTR_RO(ah_count);

int efa_sysfs_init(struct efa_dev *dev)
{
	struct device *device = &dev->pdev->dev;

	if (device_create_file(device, &dev_attr_ah_count))
		dev_err(device, "Failed to create AH count sysfs file\n");

	return 0;
}

void efa_sysfs_destroy(struct efa_dev *dev)
{
	device_remove_file(&dev->pdev->dev, &dev_attr_ah_count);
}
