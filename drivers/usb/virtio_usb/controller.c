// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * virtio_usb: VirtIO USB device
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <uapi/linux/virtio_ids.h>

#include "controller.h"
#include "device.h"
#include "host.h"
#include "otg.h"
#include "vq_common.h"

u32 virtio_usb_cmd_timeout_ms = MSEC_PER_SEC;
module_param_named(cmd_timeout_ms, virtio_usb_cmd_timeout_ms, uint, 0644);
MODULE_PARM_DESC(cmd_timeout_ms, "Command completion timeout in milliseconds");

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

	if (!virtio_has_feature(vdev, VIRTIO_USB_F_HOST) &&
	    !virtio_has_feature(vdev, VIRTIO_USB_F_DEVICE)) {
		dev_err(&vdev->dev,
			"device should support at least one usb role\n");
		return -EINVAL;
	}

	if ((!(virtio_has_feature(vdev, VIRTIO_USB_F_HOST)) ||
	     !(virtio_has_feature(vdev, VIRTIO_USB_F_DEVICE))) &&
	    virtio_has_feature(vdev, VIRTIO_USB_F_SWITCH_ROLE)) {
		dev_err(&vdev->dev,
			"OTG requires both (host and device) roles support\n");
		return -EINVAL;
	}

	if (!virtio_usb_cmd_timeout_ms) {
		dev_err(&vdev->dev, "msg_timeout_ms value cannot be zero\n");
		return -EINVAL;
	}

	return 0;
}

static void virtio_usb_remove(struct virtio_device *vdev);

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
	int rc = 0, i = 0;

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

	if (virtio_has_feature(vdev, VIRTIO_USB_F_HOST))
		vusb->host_role = 1;

	if (virtio_has_feature(vdev, VIRTIO_USB_F_DEVICE))
		vusb->device_role = 1;

	if (virtio_has_feature(vdev, VIRTIO_USB_F_SWITCH_ROLE))
		vusb->switch_role = 1;

	dev_info(&vdev->dev, "%s nports %d (h_role %d dev_role %d)\n", __func__,
		 vusb->nports, vusb->host_role, vusb->device_role);

	/* Only allocate/negotiate the virtqueue triplets this instance
	 * actually needs: HOST_* only exists when host_role is negotiated,
	 * DEV_* only when device_role is negotiated, OTG_* only when both
	 * roles are negotiated. A pure single-role instance therefore has
	 * exactly VIRTIO_USB_VQ_HOST_MAX (3) or VIRTIO_USB_VQ_DEV_MAX (3)
	 * virtqueues, not a fixed layout - queues that don't exist on the
	 * wire must not be created, since a peer with no host-role VP has
	 * no host command/event/data queues to negotiate at all.
	 */
	vusb->host_vq_base = -1;
	vusb->dev_vq_base = -1;

	if (vusb->host_role) {
		vusb->host_vq_base = nvqs;
		nvqs += VIRTIO_USB_VQ_HOST_MAX;
	}
	if (vusb->device_role) {
		vusb->dev_vq_base = nvqs;
		nvqs += VIRTIO_USB_VQ_DEV_MAX;
	}
	if (vusb->host_role && vusb->device_role) {
		/* The OTG command/event queue pair is needed whenever both
		 * roles are negotiated - it is how the driver asks each
		 * port for its actual role via otg_get_role() below, since
		 * a port's own role is otherwise ambiguous (host|device)
		 * until then.
		 */
		vusb->otg_vq_base = nvqs;
		nvqs += VIRTIO_USB_VQ_OTG_MAX;
	}

	vusb->vqueues = devm_kcalloc(&vdev->dev, nvqs, sizeof(*vusb->vqueues),
				     GFP_KERNEL);
	if (!vusb->vqueues)
		return -ENOMEM;

	vusb->nvqs = nvqs;

	if (vusb->host_vq_base >= 0)
		for (i = 0; i < VIRTIO_USB_VQ_HOST_MAX; i++) {
			vusb->vqueues[vusb->host_vq_base + i].name =
				host_vqueues[i].name;
			vusb->vqueues[vusb->host_vq_base + i].callback =
				host_vqueues[i].callback;
			vusb->vqueues[vusb->host_vq_base + i].process =
				host_vqueues[i].process;
			vusb->vqueues[vusb->host_vq_base + i].stop =
				host_vqueues[i].stop;
		}

	if (vusb->dev_vq_base >= 0)
		for (i = 0; i < VIRTIO_USB_VQ_DEV_MAX; i++) {
			vusb->vqueues[vusb->dev_vq_base + i].name =
				dev_vqueues[i].name;
			vusb->vqueues[vusb->dev_vq_base + i].callback =
				dev_vqueues[i].callback;
			vusb->vqueues[vusb->dev_vq_base + i].process =
				dev_vqueues[i].process;
			vusb->vqueues[vusb->dev_vq_base + i].stop =
				dev_vqueues[i].stop;
		}

	if (vusb->host_role && vusb->device_role)
		for (i = 0; i < VIRTIO_USB_VQ_OTG_MAX; i++) {
			vusb->vqueues[vusb->otg_vq_base + i].name =
				otg_vqueues[i].name;
			vusb->vqueues[vusb->otg_vq_base + i].callback =
				otg_vqueues[i].callback;
			vusb->vqueues[vusb->otg_vq_base + i].process =
				otg_vqueues[i].process;
			vusb->vqueues[vusb->otg_vq_base + i].stop =
				otg_vqueues[i].stop;
		}

	rc = virtio_usb_find_vqs(vusb);
	if (rc) {
		dev_err(&vdev->dev, "%s virtio_usb_find_vqs() error(%d)\n",
			__func__, rc);
		goto on_error;
	}

	if (vusb->host_role && vusb->device_role) {
		rc = otg_init(vusb);
		if (rc) {
			dev_err(&vdev->dev, "%s otg_init() error(%d)\n",
				__func__, rc);
			goto on_error;
		}
	}

	if (vusb->device_role) {
		INIT_WORK(&vusb->vq_dev_event_work, virtio_usb_dc_event_work);
		INIT_WORK(&vusb->vq_dev_data_rx_work, virtio_usb_dc_data_work);

		/* Populate the shared device event queue before telling the
		 * backend we are ready, so a DEV_CONNECTED event can never
		 * race ahead of the driver having posted receive buffers.
		 */
		rc = virtio_usb_dc_event_populate(vusb);
		if (rc) {
			dev_err(&vdev->dev,
				"%s virtio_usb_dc_event_populate() error(%d)\n",
				__func__, rc);
			goto on_error;
		}
	}

	virtio_device_ready(vdev);

	/* Resolve every port's role. With only one role negotiated, every
	 * port unambiguously has that role. With both negotiated, query
	 * each port's actual role - and, if switch_role is negotiated,
	 * whether the port supports switching - individually via
	 * otg_get_role(), since a port's own role is otherwise ambiguous
	 * (host|device).
	 */
	for (i = 0; i < vusb->nports; i++) {
		vusb->vports[i].vusb = vusb;
		spin_lock_init(&vusb->vports[i].vhc_lock);

		if (vusb->host_role && !vusb->device_role) {
			vusb->vports[i].role = VIRTIO_USB_ROLE_HOST;
		} else if (vusb->device_role && !vusb->host_role) {
			vusb->vports[i].role = VIRTIO_USB_ROLE_DEVICE;
		} else {
			u32 status, role, supported_role;

			status = otg_get_role(vusb, i, &role, &supported_role);
			if (status != VIRTIO_USB_S_OK) {
				dev_err(&vdev->dev, "%s status(%d)\n", __func__,
					status);
				rc = -EIO;
				goto on_error;
			}
			if (role != VIRTIO_USB_ROLE_HOST &&
			    role != VIRTIO_USB_ROLE_DEVICE) {
				dev_err(&vdev->dev, "%s port%d wrong role %d\n",
					__func__, i, role);
				rc = -EIO;
				goto on_error;
			}
			vusb->vports[i].role = role;
			vusb->vports[i].is_otg =
				supported_role &
				(1UL << VIRTIO_USB_F_SWITCH_ROLE);

			if (vusb->vports[i].is_otg && !vusb->switch_role) {
				dev_err(&vdev->dev,
					"%s port%d switch_role not supported\n",
					__func__, i);
				rc = -EIO;
				goto on_error;
			}
		}
	}

	for (i = 0; i < vusb->nports; i++)
		dev_info(&vdev->dev, "%s port[%d] VIRTIO_USB_F_%s is_otg %s\n",
			 __func__, i, vusb->vports[i].role ? "DEVICE" : "HOST",
			 vusb->vports[i].is_otg ? "TRUE" : "FALSE");

	if (vusb->host_role) {
		INIT_WORK(&vusb->vq_host_data_rx_work, virtio_usb_hc_rx_work);
		INIT_WORK(&vusb->vq_host_evt_work, virtio_usb_hc_evt_work);

		/* Initialize one HCD pair per host-role VP. */
		for (i = 0; i < vusb->nports; i++) {
			if (vusb->vports[i].role != VIRTIO_USB_ROLE_HOST)
				continue;
			rc = virtio_usb_hc_vp_init(vusb, i);
			if (rc) {
				dev_err(&vdev->dev,
					"%s virtio_usb_hc_vp_init() port=%d error(%d)\n",
					__func__, i, rc);
				goto on_error;
			}
		}
		/* Populate the shared host event queue once, after every
		 * VP is initialized.
		 */
		rc = virtio_usb_hc_event_populate(vusb);
		if (rc) {
			dev_err(&vdev->dev,
				"%s virtio_usb_hc_event_populate() error(%d)\n",
				__func__, rc);
			goto on_error;
		}
	}

	for (i = 0; i < vusb->nports; i++) {
		if (vusb->vports[i].is_otg ||
		    vusb->vports[i].role == VIRTIO_USB_ROLE_DEVICE) {
			rc = virtio_usb_dc_init(vusb, i);
			if (rc) {
				dev_err(&vdev->dev,
					"%s virtio_usb_dc_init() port=%d error(%d)\n",
					__func__, i, rc);
				goto on_error;
			}
		}
	}

	if (vusb->host_role && vusb->device_role)
		if (otg_sysfs_init(vusb))
			goto on_error;

	WRITE_ONCE(vusb->started, true);
	if (vusb->device_role)
		schedule_work(&vusb->vq_dev_event_work);

	dev_info(&vdev->dev, "%s returns %d\n", __func__, rc);
	return rc;

on_error:
	dev_err(&vdev->dev, "%s failed(%d)\n", __func__, rc);
	virtio_usb_remove(vdev);
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

	/* Stop queue-driven workers from scheduling new activity. */
	WRITE_ONCE(vusb->started, false);

	for (i = 0; i < vusb->nvqs; i++)
		vusb->vqueues[i].stop(vusb, &vusb->vqueues[i]);

	if (vusb->host_role && vusb->vports) {
		for (i = 0; i < (int)vusb->nports; i++)
			virtio_usb_hc_vp_deinit(vusb, i);
	}

	if (vusb->device_role && vusb->vports)
		virtio_usb_dc_deinit(vusb);

	virtio_reset_device(vdev);

	vdev->config->del_vqs(vdev);

	otg_deinit(vusb);
}

static const unsigned int virtio_usb_features[] = {
	VIRTIO_USB_F_HOST,
	VIRTIO_USB_F_DEVICE,
	VIRTIO_USB_F_SWITCH_ROLE,
};

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_USB, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_usb_driver = {
	.driver.name = KBUILD_MODNAME,
	.feature_table = virtio_usb_features,
	.feature_table_size = ARRAY_SIZE(virtio_usb_features),
	.id_table = id_table,
	.validate = virtio_usb_validate,
	.probe = virtio_usb_probe,
	.remove = virtio_usb_remove,
};

module_virtio_driver(virtio_usb_driver);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Dual-role virtio-usb controller driver");
MODULE_LICENSE("GPL");
