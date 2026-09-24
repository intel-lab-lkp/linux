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

/* Forward declaration - full definition in host.h */
struct virtio_usb_hc_vp;
/* Forward declaration - full definition in device.h */
struct virtio_usb_dc;

#define VIRTIO_USB_VQ_COMMAND_IDX 0
#define VIRTIO_USB_VQ_EVENT_IDX 1
#define VIRTIO_USB_VQ_DATA_IDX 2

#define VIRTIO_USB_VQ_HOST_MAX 3
#define VIRTIO_USB_VQ_DEV_MAX 3

/**
 * struct virtio_usb_port - Per-virtual-port state.
 * @role: Role of this port (VIRTIO_USB_ROLE_HOST or _DEVICE).
 * @vhc: Host controller - non-NULL when role is HOST.
 * @vudc: Device controller - non-NULL when role is DEVICE.
 */
struct virtio_usb_port {
	unsigned int role;
	struct virtio_usb_hc_vp *vhc;
	struct virtio_usb_dc *vudc;
};

/**
 * struct virtio_usb - VirtIO USB device.
 * @vdev: Underlying virtio device
 * @vqueues: Array of virtqueue wrappers.
 * @vports: Array of per-port structures, one entry per virtual port.
 * @nports: number of supported ports
 * @nvqs: number of virtqueues for the device
 * @host_role: flag indicating support for host role
 * @device_role: flag indicating support for device role
 * @host_vq_base: index into vqueues[] where the HOST_COMMAND/EVENT/DATA
 *                triplet starts, or -1 if this instance has no host-role
 *                VP (in which case those queues do not exist on the wire
 *                and must not be negotiated).
 * @dev_vq_base: index into vqueues[] where the DEV_COMMAND/EVENT/DATA
 *               triplet starts, or -1 if this instance has no
 *               device-role VP.
 * @vq_host_data_rx_work: Kernel work draining the host data queue, shared
 *                        across every host-role VP.
 * @vq_host_evt_work: Kernel work draining the host event queue, shared
 *                     across every host-role VP.
 * @vq_dev_data_rx_work: Kernel work draining the device data queue, shared
 *                       across every device-role port.
 * @vq_dev_event_work: Kernel work draining the device event queue, shared
 *                      across every device-role port.
 */
struct virtio_usb {
	struct virtio_device *vdev;
	struct virtio_usb_queue *vqueues;
	struct virtio_usb_port *vports;
	unsigned int nports;
	u32 nvqs;
	bool host_role;
	bool device_role;
	int host_vq_base;
	int dev_vq_base;
	struct work_struct vq_host_data_rx_work;
	struct work_struct vq_host_evt_work;
	struct work_struct vq_dev_data_rx_work;
	struct work_struct vq_dev_event_work;
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

/* Command completion timeout in milliseconds (module parameter). */
extern u32 virtio_usb_cmd_timeout_ms;

#endif /* VIRTIO_USB_CONTROLLER_H */
