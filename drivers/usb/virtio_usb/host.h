/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * virtio_usb: VirtIO USB device
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef VIRTIO_USB_HOST_H
#define VIRTIO_USB_HOST_H

#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/virtio.h>

#include <uapi/linux/usb/ch11.h>
#include <uapi/linux/usb/ch9.h>

#include "controller.h"

/**
 * struct virtio_usb_port_hs_status - High speed port wPortStatus
 * See USB 2.0 spec Table 11-21
 */
struct virtio_usb_port_hs_status {
	u16 connect : 1;
	u16 enable : 1;
	u16 suspend : 1;
	u16 over_current : 1;
	u16 reset : 1;
	u16 reserved0 : 3;
	u16 power : 1;
	u16 low_speed : 1;
	u16 high_speed : 1;
	u16 test_mode : 1;
	u16 indicator_control : 1;
	u16 reserved1 : 3;
};

/**
 * virtio_usb_port_hs_change - High speed port wPortChange
 * See USB 2.0 spec Table 11-22
 */
struct virtio_usb_port_hs_change {
	u16 connect : 1;
	u16 enable : 1;
	u16 suspend : 1;
	u16 over_current : 1;
	u16 reset : 1;
	u16 reserved : 11;
};

/**
 * struct virtio_usb_port_ss_status - Super speed port wPortStatus
 * See USB 3.1 spec Table 10-13.
 */
struct virtio_usb_port_ss_status {
	u16 connect : 1;
	u16 enable : 1;
	u16 reserved0 : 1;
	u16 over_current : 1;
	u16 reset : 1;
	u16 link_state : 4;
	u16 power : 1;
	u16 speed : 3;
	u16 reserved1 : 3;
};

/**
 * struct virtio_usb_port_ss_change - Super speed port wPortChange.
 * See USB 3.1 spec Table 10-14.
 */
struct virtio_usb_port_ss_change {
	u16 connect : 1;
	u16 reserved0 : 2;
	u16 over_current : 1;
	u16 reset : 1;
	u16 bh_reset : 1;
	u16 link_state : 1;
	u16 config_error : 1;
	u16 reserved1 : 8;
};

/**
 * struct virtio_usb_hc_port - VirtIO USB host controller port.
 *
 * One slot in a VP's root hub. Pre-allocated at VP init time and reused
 * across connect/disconnect cycles - never dynamically freed.
 *
 * @vhcd_vp: Per-VP host controller this port belongs to
 * @pending_urb_list: Pending URB list to the port
 * @speed: Speed of current device connected to the port
 * @port_id: Slot index within the VP (0..VIRTIO_USB_VP_MAX_PORTS-1)
 * @lock: Spinlock that protects fields shared by interrupt handlers and
 *        hcd operation callback
 * @hs: High speed Port Status and Port Change structure
 * @ss: Super speed Port Status and Port Change structure
 */
struct virtio_usb_hc_port {
	struct virtio_usb_hc_vp *vhcd_vp;
	struct list_head pending_urb_list;
	enum usb_device_speed speed;
	u32 port_id;
	spinlock_t lock;
	struct {
		/* USB 2.0 port status bits */
		union {
			struct virtio_usb_port_hs_status bits;
			u16 value;
		} status;
		/* USB 2.0 port status change bits */
		union {
			struct virtio_usb_port_hs_change bits;
			u16 value;
		} change;
	} hs;
	struct {
		/* USB 3.0 port status bits */
		union {
			struct virtio_usb_port_ss_status bits;
			u16 value;
		} status;
		/* USB 3.0 port status change bits */
		union {
			struct virtio_usb_port_ss_change bits;
			u16 value;
		} change;
	} ss;
};

/**
 * struct virtio_usb_hc_vp - Per-VP VirtIO USB Host controller.
 *
 * One instance per host-role virtual port (physical USB socket).
 * Pointed to by virtio_usb_port.vhc - NULL for device-role VPs,
 * mirroring how virtio_usb_port.vudc works for device-role VPs.
 *
 * All VIRTIO_USB_VP_MAX_PORTS port structs are pre-allocated at init
 * time and reused across connect/disconnect cycles.
 *
 * @vusb:         VirtIO usb device
 * @vp_idx:       Index of this VP in vusb->vports[]
 * @hs:           High speed usb_hcd for this VP
 * @ss:           Super speed usb_hcd for this VP
 * @ports:        Pre-allocated port state array, one entry per slot
 * @hcqs:         Host virtqueue wrappers, indexed by VIRTIO_USB_VQ_*_IDX
 *                (shared across VPs)
 * @lock:         Spinlock protecting HC state for this VP
 */
struct virtio_usb_hc_vp {
	struct virtio_usb *vusb;
	unsigned int vp_idx;
	struct usb_hcd *hs;
	struct usb_hcd *ss;
	struct virtio_usb_hc_port ports[VIRTIO_USB_VP_MAX_PORTS];
	struct virtio_usb_queue *hcqs[VIRTIO_USB_VQ_HOST_MAX];
	spinlock_t lock;
};

extern const struct virtio_usb_vq_desc host_vqueues[VIRTIO_USB_VQ_HOST_MAX];

/**
 * vhcd_get() - Get pointer to per-VP host controller stored in hcd_priv.
 * @hcd: usb_hcd
 */
static inline struct virtio_usb_hc_vp *vhcd_get(struct usb_hcd *hcd)
{
	return ((void **)hcd->hcd_priv)[0];
}

/**
 * vhcd_set() - Store per-VP host controller pointer in hcd_priv.
 * @hcd: usb_hcd
 * @vhcd_vp: per-VP host controller
 */
static inline void vhcd_set(struct usb_hcd *hcd,
			    struct virtio_usb_hc_vp *vhcd_vp)
{
	((void **)hcd->hcd_priv)[0] = vhcd_vp;
}

/**
 * port_vhcd_get() - Get the usb_hcd that owns this port's speed.
 * @port: VirtIO usb host controller port
 *
 * Returns the SS hcd for SuperSpeed and SuperSpeed+ devices,
 * HS hcd for everything else.
 */
static inline struct usb_hcd *port_vhcd_get(struct virtio_usb_hc_port *port)
{
	return (port->speed == USB_SPEED_SUPER ||
		port->speed == USB_SPEED_SUPER_PLUS) ?
		       port->vhcd_vp->ss :
		       port->vhcd_vp->hs;
}

int virtio_usb_hc_vp_init(struct virtio_usb *vusb, unsigned int vp_idx);
int virtio_usb_hc_vp_deinit(struct virtio_usb *vusb, unsigned int vp_idx);
int virtio_usb_hc_event_populate(struct virtio_usb *vusb);
void virtio_usb_hc_rx_work(struct work_struct *work);
void virtio_usb_hc_evt_work(struct work_struct *work);

#endif
