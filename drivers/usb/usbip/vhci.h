// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2003-2008 Takahiro Hirofuchi
 * Copyright (C) 2015 Nobuo Iwata
 */

#ifndef __USBIP_VHCI_H
#define __USBIP_VHCI_H

#include <linux/device.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/wait.h>

struct vhci_device {
	struct usb_device *udev;

	/*
	 * devid specifies a remote usb device uniquely instead
	 * of combination of busnum and devnum.
	 */
	__u32 devid;

	/* speed of a remote device */
	enum usb_device_speed speed;

	/* vhci root-hub port to which this device is attached */
	__u32 rhport;

	struct usbip_device ud;

	/* lock for the below link lists */
	spinlock_t priv_lock;

	/* vhci_priv is linked to one of them. */
	struct list_head priv_tx;
	struct list_head priv_rx;

	/* vhci_unlink is linked to one of them */
	struct list_head unlink_tx;
	struct list_head unlink_rx;

	/* vhci_tx thread sleeps for this queue */
	wait_queue_head_t waitq_tx;
};

/* urb->hcpriv, use container_of() */
struct vhci_priv {
	unsigned long seqnum;
	struct list_head list;

	struct vhci_device *vdev;
	struct urb *urb;
};

struct vhci_unlink {
	/* seqnum of this request */
	unsigned long seqnum;

	struct list_head list;

	/* seqnum of the unlink target */
	unsigned long unlink_seqnum;
};

enum hub_speed {
	HUB_SPEED_HIGH = 0,
	HUB_SPEED_SUPER,
};

/* Number of supported ports. Value has an upperbound of USB_SS_MAXPORTS */
#define VHCI_DEFAULT_HC_PORTS 8
#define VHCI_MAX_HC_PORTS USB_SS_MAXPORTS

/*
 * Number of supported virtual host controllers. Value has upperbound of
 * maximum possible usb busses.
 * It is limited by a bus ID allocation in [1 .. USB_MAXBUS - 1] range,
 * resulting in maximum of USB_MAXBUS - 1 usb busses allocated.
 * Additionally, each virtual host controller registers 2 usb hubs (USB2.0
 * & USB3.0), therefore maximum number of virtual host controllers is:
 * (USB_MAXBUS - 1) / 2
 */
#define VHCI_DEFAULT_NR_HCS 1
#define VHCI_MAX_NR_HCS 31

#define MAX_STATUS_NAME 16

struct vhci {
	spinlock_t lock;

	struct platform_device *pdev;

	struct vhci_hcd *vhci_hcd_hs;
	struct vhci_hcd *vhci_hcd_ss;
};

/* for usb_hcd.hcd_priv[0] */
struct vhci_hcd {
	struct vhci *vhci;

	u32 port_status[VHCI_MAX_HC_PORTS];

	unsigned resuming:1;
	unsigned long re_timeout;

	atomic_t seqnum;

	/*
	 * NOTE:
	 * wIndex shows the port number and begins from 1.
	 * But, the index of this array begins from 0.
	 */
	struct vhci_device vdev[VHCI_MAX_HC_PORTS];
};

extern unsigned int vhci_num_controllers;
extern unsigned int vhci_hc_ports;
extern struct vhci *vhcis;
extern struct attribute_group vhci_attr_group;

/* Each VHCI has 2 hubs (USB2 and USB3), each has vhci_hc_ports ports */
#define VHCI_PORTS	(vhci_hc_ports * 2)

/* vhci_hcd.c */
void rh_port_connect(struct vhci_device *vdev, enum usb_device_speed speed);

/* vhci_sysfs.c */
int vhci_init_attr_group(void);
void vhci_finish_attr_group(void);

/* vhci_rx.c */
struct urb *pickup_urb_and_free_priv(struct vhci_device *vdev, __u32 seqnum);
int vhci_rx_loop(void *data);

/* vhci_tx.c */
int vhci_tx_loop(void *data);

static inline __u32 port_to_rhport(__u32 port)
{
	return port % vhci_hc_ports;
}

static inline int port_to_pdev_nr(__u32 port)
{
	return port / VHCI_PORTS;
}

static inline struct vhci_hcd *hcd_to_vhci_hcd(struct usb_hcd *hcd)
{
	return (struct vhci_hcd *) (hcd->hcd_priv);
}

static inline struct device *hcd_dev(struct usb_hcd *hcd)
{
	return (hcd)->self.controller;
}

static inline const char *hcd_name(struct usb_hcd *hcd)
{
	return (hcd)->self.bus_name;
}

static inline struct usb_hcd *vhci_hcd_to_hcd(struct vhci_hcd *vhci_hcd)
{
	return container_of((void *) vhci_hcd, struct usb_hcd, hcd_priv);
}

static inline struct vhci_hcd *vdev_to_vhci_hcd(struct vhci_device *vdev)
{
	return container_of((void *)(vdev - vdev->rhport), struct vhci_hcd, vdev);
}

#endif /* __USBIP_VHCI_H */
