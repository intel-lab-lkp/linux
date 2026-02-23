// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/iommu.h>
#include <linux/slab.h>
#include "qda_drv.h"
#include "qda_cb.h"

static void qda_cb_dev_release(struct device *dev)
{
	kfree(dev);
}

static int qda_configure_cb_iommu(struct device *cb_dev, struct device_node *cb_node)
{
	int ret;

	qda_dbg(NULL, "Configuring DMA/IOMMU for CB device %s\n", dev_name(cb_dev));

	/* Use of_dma_configure which handles both DMA and IOMMU configuration */
	ret = of_dma_configure(cb_dev, cb_node, true);
	if (ret) {
		qda_err(NULL, "of_dma_configure failed for %s: %d\n", dev_name(cb_dev), ret);
		return ret;
	}

	qda_dbg(NULL, "DMA/IOMMU configured successfully for CB device %s\n", dev_name(cb_dev));
	return 0;
}

static int qda_cb_setup_device(struct qda_dev *qdev, struct device *cb_dev)
{
	int rc;
	u32 sid, pa_bits = 32;

	qda_dbg(qdev, "Setting up CB device %s\n", dev_name(cb_dev));

	if (of_property_read_u32(cb_dev->of_node, "reg", &sid)) {
		qda_dbg(qdev, "No 'reg' property found, defaulting SID to 0\n");
		sid = 0;
	}

	rc = dma_set_mask(cb_dev, DMA_BIT_MASK(pa_bits));
	if (rc) {
		qda_err(qdev, "%d bit DMA enable failed: %d\n", pa_bits, rc);
		return rc;
	}

	qda_dbg(qdev, "CB device setup complete - SID: %u, PA bits: %u\n", sid, pa_bits);

	return 0;
}

int qda_create_cb_device(struct qda_dev *qdev, struct device_node *cb_node)
{
	struct device *cb_dev;
	int ret;
	u32 sid = 0;
	struct qda_cb_dev *entry;

	qda_dbg(qdev, "Creating CB device for node: %s\n", cb_node->name);

	of_property_read_u32(cb_node, "reg", &sid);

	cb_dev = kzalloc_obj(*cb_dev, GFP_KERNEL);
	if (!cb_dev)
		return -ENOMEM;

	device_initialize(cb_dev);
	cb_dev->parent = qdev->dev;
	cb_dev->bus = &qda_cb_bus_type; /* Use our custom bus type for IOMMU handling */
	cb_dev->release = qda_cb_dev_release;
	dev_set_name(cb_dev, "qda-cb-%s-%u", qdev->dsp_name, sid);

	qda_dbg(qdev, "Initialized CB device: %s\n", dev_name(cb_dev));

	cb_dev->of_node = of_node_get(cb_node);

	cb_dev->dma_mask = &cb_dev->coherent_dma_mask;
	cb_dev->coherent_dma_mask = DMA_BIT_MASK(32);

	dev_set_drvdata(cb_dev->parent, qdev);

	ret = device_add(cb_dev);
	if (ret) {
		qda_err(qdev, "Failed to add CB device for SID %u: %d\n", sid, ret);
		goto cleanup_device_init;
	}

	qda_dbg(qdev, "CB device added to system\n");

	ret = qda_configure_cb_iommu(cb_dev, cb_node);
	if (ret) {
		qda_err(qdev, "IOMMU configuration failed: %d\n", ret);
		goto cleanup_device_add;
	}

	ret = qda_cb_setup_device(qdev, cb_dev);
	if (ret) {
		qda_err(qdev, "CB device setup failed: %d\n", ret);
		goto cleanup_device_add;
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		ret = -ENOMEM;
		goto cleanup_device_add;
	}

	entry->dev = cb_dev;
	list_add_tail(&entry->node, &qdev->cb_devs);

	qda_dbg(qdev, "Successfully created CB device for SID %u\n", sid);
	return 0;

cleanup_device_add:
	device_del(cb_dev);
cleanup_device_init:
	of_node_put(cb_dev->of_node);
	put_device(cb_dev);
	return ret;
}

void qda_destroy_cb_device(struct device *cb_dev)
{
	struct iommu_group *group;

	if (!cb_dev) {
		qda_dbg(NULL, "NULL CB device passed to destroy\n");
		return;
	}

	qda_dbg(NULL, "Destroying CB device %s\n", dev_name(cb_dev));

	group = iommu_group_get(cb_dev);
	if (group) {
		qda_dbg(NULL, "Removing %s from IOMMU group\n", dev_name(cb_dev));
		iommu_group_remove_device(cb_dev);
		iommu_group_put(group);
	}

	of_node_put(cb_dev->of_node);
	cb_dev->of_node = NULL;
	device_unregister(cb_dev);

	qda_dbg(NULL, "CB device %s destroyed\n", dev_name(cb_dev));
}
