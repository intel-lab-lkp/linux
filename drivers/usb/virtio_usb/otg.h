// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * virtio_usb: VirtIO USB device
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef VIRTIO_USB_OTG_H
#define VIRTIO_USB_OTG_H

#include <linux/usb/role.h>
#include "controller.h"

extern int otg_init(struct virtio_usb *vusb);
extern int otg_sysfs_init(struct virtio_usb *vusb);
extern void otg_deinit(struct virtio_usb *vusb);
extern u32 otg_get_role(struct virtio_usb *vusb, int port_id, u32 *role,
			u32 *supported_role);

struct virtio_usb_port_otg {
	struct virtio_usb *vusb;
	struct mutex lock;
	struct usb_role_switch *sw;
	char *sysfs_name;
	struct work_struct set_role_work;
	int port_id;
};

struct virtio_usb_otg {
	struct virtio_usb *vusb;
	struct mutex lock;
	struct virtio_usb_queue *oqs[VIRTIO_USB_VQ_OTG_MAX];
	struct work_struct event_process_work;
};

extern const struct virtio_usb_vq_desc otg_vqueues[VIRTIO_USB_VQ_OTG_MAX];

#endif /* VIRTIO_USB_OTG_H */
