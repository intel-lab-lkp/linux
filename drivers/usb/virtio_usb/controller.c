// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * virtio_usb: VirtIO USB device
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/module.h>
#include <uapi/linux/virtio_ids.h>

#include "controller.h"

/**
 * virtio_usb_find_vqs() - Enumerate and initialize all virtqueues.
 * @vusb: VirtIO usb device.
 *
 * Context: Any context.
 * Return: 0 on success, -errno on failure.
 */
static int virtio_usb_find_vqs(struct virtio_usb *vusb)
{
	unsigned int nvqs = vusb->nvqs, i;
	struct virtqueue **vqs = NULL;
	int rc = -ENOMEM;

	vqs = kcalloc(nvqs, sizeof(void *), GFP_KERNEL);
	if (!vqs)
		return -ENOMEM;

	struct virtqueue_info *vqs_info =
		kcalloc(nvqs, sizeof(*vqs_info), GFP_KERNEL);
	if (!vqs_info) {
		rc = -ENOMEM;
		goto on_exit;
	}
	for (i = 0; i < nvqs; i++) {
		vqs_info[i].name = vusb->vqueues[i].name;
		vqs_info[i].callback = vusb->vqueues[i].callback;
	}
	rc = virtio_find_vqs(vusb->vdev, nvqs, vqs, vqs_info, NULL);

	kfree(vqs_info);

	for (i = 0; i < nvqs; i++) {
		vusb->vqueues[i].vqueue = vqs[i];
		spin_lock_init(&vusb->vqueues[i].lock);
	}
on_exit:
	kfree(vqs);
	return rc;
}

/**
 * virtio_usb_validate() - Validate if the device can be started.
 * @vdev: VirtIO parent device.
 *
 * Context: Any context.
 * Return: 0 on success, -EINVAL on failure.
 */
static int virtio_usb_validate(struct virtio_device *vdev)
{
	if (!vdev->config->get) {
		dev_err(&vdev->dev, "configuration access disabled\n");
		return -EINVAL;
	}

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1)) {
		dev_err(&vdev->dev,
			"device does not comply with spec version 1.x\n");
		return -EINVAL;
	}

	return 0;
}

/**
 * virtio_usb_probe() - Probe VirtIO usb controller.
 * @vdev: VirtIO parent device.
 *
 * Context: Any context that permits to sleep.
 * Return: 0 on success, -errno on failure.
 */
static int virtio_usb_probe(struct virtio_device *vdev)
{
	struct virtio_usb *vusb;
	unsigned int nvqs = 0;
	int rc = 0;

	vusb = devm_kzalloc(&vdev->dev, sizeof(*vusb), GFP_KERNEL);
	if (!vusb)
		return -ENOMEM;

	vusb->vdev = vdev;
	vdev->priv = vusb;

	virtio_cread_le(vdev, struct virtio_usb_config, ports, &vusb->nports);

	vusb->vports = devm_kcalloc(&vdev->dev, vusb->nports,
				    sizeof(*vusb->vports), GFP_KERNEL);
	if (!vusb->vports)
		return -ENOMEM;

	vusb->vqueues = devm_kcalloc(&vdev->dev, nvqs, sizeof(*vusb->vqueues),
				     GFP_KERNEL);
	if (!vusb->vqueues)
		return -ENOMEM;

	vusb->nvqs = nvqs;

	rc = virtio_usb_find_vqs(vusb);
	if (rc)
		goto on_exit;

	virtio_device_ready(vdev);

on_exit:
	return rc;
}

/**
 * virtio_usb_remove() - Remove VirtIO usb device.
 * @vdev: VirtIO parent device.
 *
 * Context: Any context that permits to sleep.
 */
static void virtio_usb_remove(struct virtio_device *vdev)
{
	struct virtio_usb *vusb = vdev->priv;
	int i;

	virtio_reset_device(vdev);
	for (i = 0; i < vusb->nvqs; i++)
		vusb->vqueues[i].stop(vusb, &vusb->vqueues[i]);

	vdev->config->del_vqs(vdev);
}

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_USB, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_usb_driver = {
	.driver.name = KBUILD_MODNAME,
	.id_table = id_table,
	.validate = virtio_usb_validate,
	.probe = virtio_usb_probe,
	.remove = virtio_usb_remove,
};

module_virtio_driver(virtio_usb_driver);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Dual-role virtio-usb controller driver");
MODULE_LICENSE("GPL");
