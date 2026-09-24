/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * virtio-usb: Virtio usb device
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef VIRTIO_USB_CONTROLLER_H
#define VIRTIO_USB_CONTROLLER_H

#include <linux/slab.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>

#include <uapi/linux/virtio_usb.h>

/**
 * struct virtio_usb_port - Per-virtual-port state.
 * @role: Role of this port (VIRTIO_USB_ROLE_HOST or _DEVICE), once a
 *        role-providing commit has assigned it. Unused for now - this
 *        struct is deliberately introduced ahead of any code that
 *        populates or reads @role, so that every later commit that adds
 *        a role (host, device, OTG) can build on this same per-port
 *        array from the start instead of each reinventing its own
 *        port-indexed storage.
 */
struct virtio_usb_port {
	unsigned int role;
};

/**
 * struct virtio_usb - VirtIO USB device.
 * @vdev: Underlying virtio device
 * @vqueues: Array of virtqueue wrappers.
 * @vports: Array of per-port structures, one entry per virtual port.
 * @nports: number of supported ports
 * @nvqs: number of virtqueues for the device
 */
struct virtio_usb {
	struct virtio_device *vdev;
	struct virtio_usb_queue *vqueues;
	struct virtio_usb_port *vports;
	unsigned int nports;
	u32 nvqs;
};

/**
 * struct virtio_usb_queue - Virtqueue wrapper structure.
 * @name: Name of the virtqueue.
 * @callback: Used to synchronize access to a virtqueue.
 * @vqueue: Underlying virtqueue.
 * @lock: Used to synchronize access to a virtqueue.
 * @process: queue process callback function
 * @stop: queue stop callback function
 */
struct virtio_usb_queue {
	const char *name;
	vq_callback_t *callback;
	struct virtqueue *vqueue;
	spinlock_t lock;
	void (*process)(struct virtio_usb *vusb, void *buf);
	void (*stop)(struct virtio_usb *vusb, struct virtio_usb_queue *vq);
};

/**
 * struct virtio_usb_vq_desc - Read-only virtqueue template.
 * @name: Name of the virtqueue.
 * @callback: Virtqueue notification callback function.
 * @process: queue process callback function.
 * @stop: queue stop callback function.
 *
 * Immutable per-role virtqueue description, copied into a fresh
 * per-instance struct virtio_usb_queue during probe() instead of being
 * shared directly - so that multiple probed virtio_usb instances never
 * alias the same struct virtqueue.
 */
struct virtio_usb_vq_desc {
	const char *name;
	vq_callback_t *callback;
	void (*process)(struct virtio_usb *vusb, void *buf);
	void (*stop)(struct virtio_usb *vusb, struct virtio_usb_queue *vq);
};

#endif /* VIRTIO_USB_CONTROLLER_H */
