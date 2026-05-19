// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/iommu.h>
#include <linux/qda_compute_bus.h>
#include <linux/slab.h>
#include <drm/drm_print.h>
#include "qda_drv.h"
#include "qda_cb.h"

int qda_create_cb_device(struct qda_dev *qdev, struct device_node *cb_node)
{
	struct device *cb_dev;
	u32 sid = 0;
	char name[64];
	struct qda_cb_dev *entry;

	drm_dbg_driver(&qdev->drm_dev, "Creating CB device for node: %s\n", cb_node->name);

	of_property_read_u32(cb_node, "reg", &sid);

	snprintf(name, sizeof(name), "qda-cb-%s-%u", qdev->dsp_name, sid);

	cb_dev = create_qda_cb_device(qdev->dev, name, DMA_BIT_MASK(32), cb_node);
	if (IS_ERR(cb_dev)) {
		drm_err(&qdev->drm_dev, "Failed to create CB device for SID %u: %ld\n",
			sid, PTR_ERR(cb_dev));
		return PTR_ERR(cb_dev);
	}

	entry = kzalloc_obj(*entry);
	if (!entry) {
		device_unregister(cb_dev);
		return -ENOMEM;
	}

	entry->dev = cb_dev;
	list_add_tail(&entry->node, &qdev->cb_devs);

	drm_dbg_driver(&qdev->drm_dev, "Successfully created CB device for SID %u\n", sid);
	return 0;
}

void qda_cb_unpopulate(struct qda_dev *qdev)
{
	struct qda_cb_dev *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &qdev->cb_devs, node) {
		list_del(&entry->node);
		qda_destroy_cb_device(entry->dev);
		kfree(entry);
	}
}

int qda_cb_populate(struct qda_dev *qdev, struct device_node *parent_node)
{
	struct device_node *child;
	int count = 0, success = 0;

	for_each_child_of_node(parent_node, child) {
		if (of_device_is_compatible(child, "qcom,fastrpc-compute-cb")) {
			count++;
			if (qda_create_cb_device(qdev, child) == 0) {
				success++;
				dev_dbg(qdev->dev, "Created CB device for node: %s\n",
					child->name);
			} else {
				dev_err(qdev->dev, "Failed to create CB device for: %s\n",
					child->name);
			}
		}
	}
	if (count == 0)
		return 0;
	return success > 0 ? 0 : -ENODEV;
}

void qda_destroy_cb_device(struct device *cb_dev)
{
	struct iommu_group *group;

	if (!cb_dev) {
		pr_debug("qda: NULL CB device passed to destroy\n");
		return;
	}

	dev_dbg(cb_dev, "Destroying CB device %s\n", dev_name(cb_dev));

	group = iommu_group_get(cb_dev);
	if (group) {
		dev_dbg(cb_dev, "Removing %s from IOMMU group\n", dev_name(cb_dev));
		iommu_group_remove_device(cb_dev);
		iommu_group_put(group);
	}

	device_unregister(cb_dev);
}
