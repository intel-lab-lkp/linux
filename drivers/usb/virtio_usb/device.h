/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * virtio-usb: Virtio usb device
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef VIRTIO_USB_DC_H
#define VIRTIO_USB_DC_H

#include <linux/usb.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/wait.h>
#include <uapi/linux/usb/ch11.h>
#include <uapi/linux/usb/ch9.h>
#include <linux/usb/gadget.h>

#include "vq_common.h"

#include <uapi/linux/virtio_usb.h>

#define VIRTIO_USB_EP_CAPS_TYPE_CONTROL 0x01
#define VIRTIO_USB_EP_CAPS_TYPE_INTERRUPT 0x02
#define VIRTIO_USB_EP_CAPS_TYPE_BULK 0x04
#define VIRTIO_USB_EP_CAPS_TYPE_ISOCHRONOUS 0x08

#define VIRTIO_USB_EP_CAPS_DIR_IN 0x02
#define VIRTIO_USB_EP_CAPS_DIR_OUT 0x01

#define VIRTIO_USB_EP_CAPS(_type, _dir)                                        \
	{                                                                      \
		.type_control = !!((_type) & VIRTIO_USB_EP_CAPS_TYPE_CONTROL), \
		.type_iso = !!((_type) & VIRTIO_USB_EP_CAPS_TYPE_ISOCHRONOUS), \
		.type_bulk = !!((_type) & VIRTIO_USB_EP_CAPS_TYPE_BULK),       \
		.type_int = !!((_type) & VIRTIO_USB_EP_CAPS_TYPE_INTERRUPT),   \
		.dir_in = !!((_dir) & VIRTIO_USB_EP_CAPS_DIR_IN),              \
		.dir_out = !!((_dir) & VIRTIO_USB_EP_CAPS_DIR_OUT),            \
	}

enum vep_state { EP_DISABLED, EP_ENABLING, EP_ENABLED, EP_HALTED };

/**
 * struct virtio_usb_ep - virtio usb device endpoint
 * @ep: usb ep
 * @vusb: VirtIO usb device
 * @setup: setup packet for control endpoint
 * @req_queue: list of usb requests submitted to ep awaiting response
 * @pend_queue: list of usb requests waiting for submit while EP is ENABLING
 * @state: endpoint state (necessary to support split-phase async processing)
 * @enable_work:  workstruct for the async ep_enable()
 * @disable_work:  workstruct for the async ep_disable()
 * @last_err: error code if async processeing brings us to HALTED state
 * @ep_id: Id of the endpoint
 * @name: Endpoint name
 */
struct virtio_usb_ep {
	struct usb_ep ep;
	struct virtio_usb_dc *vudc;
	struct usb_ctrlrequest setup;
	struct list_head req_queue;
	struct list_head pend_queue;
	enum vep_state state;
	struct work_struct enable_work;
	struct work_struct disable_work;
	int last_err;
	u16 ep_id;
	char name[16];
};

/**
 * struct virtio_usb_dc - VirtIO USB Device controller (USBDC) device.
 * @gadget: Pointer to the usb gadget structure
 * @driver: Pointer to the usb gadget  driver structure
 * @veps: Virtual endpoints
 * @neps: Number of virtual endpoints
 * @dcqs: Device virtqueue wrappers, indexed by VIRTIO_USB_VQ_*_IDX.
 * @vusb: VirtIO usb device
 * @registered: Flag indicating registration status to the UDC core.
 * @pullup:  Software-controlled connect/disconnect status USB host.
 * @lock: Spinlock that protects device state
 * @ep0_idle: Wait queue for SETUP serialization while ep0 has an in-flight req
 */
struct virtio_usb_dc {
	struct platform_device *pdev;
	u16 port; // Device id
	struct usb_gadget gadget;
	struct usb_gadget_driver *driver;
	struct virtio_usb_ep *veps;
	u32 neps;
	struct virtio_usb_queue *dcqs[VIRTIO_USB_VQ_DEV_MAX];
	struct virtio_usb *vusb;
	unsigned registered : 1;
	unsigned pullup : 1;
	spinlock_t lock;
	wait_queue_head_t ep0_idle;
	/* Workqueue for EP enable/disable/cancel commands */
	struct workqueue_struct *ep_cmd_wq;
	struct workqueue_struct *dc_cmd_wq;
	struct work_struct dc_cmd_work;
	struct mutex dc_cmd_work_lock;
	atomic_t pending_self_pwr;
	atomic_t pending_vbus_ma;
	unsigned int cur_vbus_ma;
	bool cur_self_pwr;
};

extern const struct virtio_usb_vq_desc dev_vqueues[VIRTIO_USB_VQ_DEV_MAX];

int virtio_usb_dc_init(struct virtio_usb *vusb, int port_id);
int virtio_usb_dc_deinit(struct virtio_usb *vusb);
void virtio_usb_dc_event_work(struct work_struct *work);
void virtio_usb_dc_data_work(struct work_struct *work);
int virtio_usb_dc_event_populate(struct virtio_usb *vusb);
#endif /* VIRTIO_USB_DC_H */
