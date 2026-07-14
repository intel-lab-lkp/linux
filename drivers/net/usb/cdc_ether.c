// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CDC Ethernet based networking peripherals
 * Copyright (C) 2003-2005 by David Brownell
 * Copyright (C) 2006 by Ole Andre Vadla Ravnas (ActiveSync)
 */

// #define	DEBUG			// error path messages, extra info
// #define	VERBOSE			// more; success messages

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/workqueue.h>
#include <linux/mii.h>
#include <linux/usb.h>
#include <linux/usb/cdc.h>
#include <linux/usb/usbnet.h>


static int usbnet_cdc_zte_bind(struct usbnet *dev, struct usb_interface *intf)
{
	int status = usbnet_cdc_bind(dev, intf);

	if (!status && (dev->net->dev_addr[0] & 0x02))
		eth_hw_addr_random(dev->net);

	return status;
}

/* Ensure correct link state
 *
 * Some devices (ZTE MF823/831/910) export two carrier on notifications when
 * connected. This causes the link state to be incorrect. Work around this by
 * always setting the state to off, then on.
 */
static void usbnet_cdc_zte_status(struct usbnet *dev, struct urb *urb)
{
	struct usb_cdc_notification *event;

	if (urb->actual_length < sizeof(*event))
		return;

	event = urb->transfer_buffer;

	if (event->bNotificationType != USB_CDC_NOTIFY_NETWORK_CONNECTION) {
		usbnet_cdc_status(dev, urb);
		return;
	}

	netif_dbg(dev, timer, dev->net, "CDC: carrier %s\n",
		  event->wValue ? "on" : "off");

	if (event->wValue &&
	    netif_carrier_ok(dev->net))
		netif_carrier_off(dev->net);

	usbnet_link_change(dev, !!event->wValue, 0);
}

static const struct driver_info	cdc_info = {
	.description =	"CDC Ethernet Device",
	.flags =	FLAG_ETHER | FLAG_POINTTOPOINT,
	.bind =		usbnet_cdc_bind,
	.unbind =	usbnet_cdc_unbind,
	.status =	usbnet_cdc_status,
	.set_rx_mode =	usbnet_cdc_update_filter,
	.manage_power =	usbnet_manage_power,
};

static const struct driver_info	zte_cdc_info = {
	.description =	"ZTE CDC Ethernet Device",
	.flags =	FLAG_ETHER | FLAG_POINTTOPOINT,
	.bind =		usbnet_cdc_zte_bind,
	.unbind =	usbnet_cdc_unbind,
	.status =	usbnet_cdc_zte_status,
	.set_rx_mode =	usbnet_cdc_update_filter,
	.manage_power =	usbnet_manage_power,
	.rx_fixup = usbnet_cdc_zte_rx_fixup,
};

static const struct driver_info wwan_info = {
	.description =	"Mobile Broadband Network Device",
	.flags =	FLAG_WWAN,
	.bind =		usbnet_cdc_bind,
	.unbind =	usbnet_cdc_unbind,
	.status =	usbnet_cdc_status,
	.set_rx_mode =	usbnet_cdc_update_filter,
	.manage_power =	usbnet_manage_power,
};

/*-------------------------------------------------------------------------*/

#define HUAWEI_VENDOR_ID	0x12D1
#define NOVATEL_VENDOR_ID	0x1410
#define ZTE_VENDOR_ID		0x19D2
#define DELL_VENDOR_ID		0x413C
#define REALTEK_VENDOR_ID	0x0bda
#define SAMSUNG_VENDOR_ID	0x04e8
#define LENOVO_VENDOR_ID	0x17ef
#define LINKSYS_VENDOR_ID	0x13b1
#define NVIDIA_VENDOR_ID	0x0955
#define HP_VENDOR_ID		0x03f0
#define MICROSOFT_VENDOR_ID	0x045e
#define UBLOX_VENDOR_ID		0x1546
#define TPLINK_VENDOR_ID	0x2357
#define AQUANTIA_VENDOR_ID	0x2eca
#define ASIX_VENDOR_ID		0x0b95

static const struct usb_device_id	products[] = {
/* BLACKLIST !!
 *
 * First blacklist any products that are egregiously nonconformant
 * with the CDC Ethernet specs.  Minor braindamage we cope with; when
 * they're not even trying, needing a separate driver is only the first
 * of the differences to show up.
 */

#define	ZAURUS_MASTER_INTERFACE \
	.bInterfaceClass	= USB_CLASS_COMM, \
	.bInterfaceSubClass	= USB_CDC_SUBCLASS_ETHERNET, \
	.bInterfaceProtocol	= USB_CDC_PROTO_NONE

#define ZAURUS_FAKE_INTERFACE \
	.bInterfaceClass	= USB_CLASS_COMM, \
	.bInterfaceSubClass	= USB_CDC_SUBCLASS_MDLM, \
	.bInterfaceProtocol	= USB_CDC_PROTO_NONE

/* SA-1100 based Sharp Zaurus ("collie"), or compatible;
 * wire-incompatible with true CDC Ethernet implementations.
 * (And, it seems, needlessly so...)
 */
{
	.match_flags	=   USB_DEVICE_ID_MATCH_INT_INFO
			  | USB_DEVICE_ID_MATCH_DEVICE,
	.idVendor		= 0x04DD,
	.idProduct		= 0x8004,
	ZAURUS_MASTER_INTERFACE,
	.driver_info		= 0,
},

/* PXA-25x based Sharp Zaurii.  Note that it seems some of these
 * (later models especially) may have shipped only with firmware
 * advertising false "CDC MDLM" compatibility ... but we're not
 * clear which models did that, so for now let's assume the worst.
 */
{
	.match_flags	=   USB_DEVICE_ID_MATCH_INT_INFO
			  | USB_DEVICE_ID_MATCH_DEVICE,
	.idVendor		= 0x04DD,
	.idProduct		= 0x8005,	/* A-300 */
	ZAURUS_MASTER_INTERFACE,
	.driver_info		= 0,
}, {
	.match_flags	=   USB_DEVICE_ID_MATCH_INT_INFO
			  | USB_DEVICE_ID_MATCH_DEVICE,
	.idVendor		= 0x04DD,
	.idProduct		= 0x8005,   /* A-300 */
	ZAURUS_FAKE_INTERFACE,
	.driver_info        = 0,
}, {
	.match_flags    =   USB_DEVICE_ID_MATCH_INT_INFO
			  | USB_DEVICE_ID_MATCH_DEVICE,
	.idVendor		= 0x04DD,
	.idProduct		= 0x8006,	/* B-500/SL-5600 */
	ZAURUS_MASTER_INTERFACE,
	.driver_info		= 0,
}, {
	.match_flags    =   USB_DEVICE_ID_MATCH_INT_INFO
			  | USB_DEVICE_ID_MATCH_DEVICE,
	.idVendor		= 0x04DD,
	.idProduct		= 0x8006,   /* B-500/SL-5600 */
	ZAURUS_FAKE_INTERFACE,
	.driver_info        = 0,
}, {
	.match_flags    =   USB_DEVICE_ID_MATCH_INT_INFO
			  | USB_DEVICE_ID_MATCH_DEVICE,
	.idVendor		= 0x04DD,
	.idProduct		= 0x8007,	/* C-700 */
	ZAURUS_MASTER_INTERFACE,
	.driver_info		= 0,
}, {
	.match_flags    =   USB_DEVICE_ID_MATCH_INT_INFO
			  | USB_DEVICE_ID_MATCH_DEVICE,
	.idVendor		= 0x04DD,
	.idProduct		= 0x8007,   /* C-700 */
	ZAURUS_FAKE_INTERFACE,
	.driver_info        = 0,
}, {
	.match_flags    =   USB_DEVICE_ID_MATCH_INT_INFO
		 | USB_DEVICE_ID_MATCH_DEVICE,
	.idVendor               = 0x04DD,
	.idProduct              = 0x9031,	/* C-750 C-760 */
	ZAURUS_MASTER_INTERFACE,
	.driver_info		= 0,
}, {
	.match_flags    =   USB_DEVICE_ID_MATCH_INT_INFO
		 | USB_DEVICE_ID_MATCH_DEVICE,
	.idVendor               = 0x04DD,
	.idProduct              = 0x9032,	/* SL-6000 */
	ZAURUS_MASTER_INTERFACE,
	.driver_info		= 0,
}, {
	.match_flags    =   USB_DEVICE_ID_MATCH_INT_INFO
		 | USB_DEVICE_ID_MATCH_DEVICE,
	.idVendor               = 0x04DD,
	.idProduct              = 0x9032,	/* SL-6000 */
	ZAURUS_FAKE_INTERFACE,
	.driver_info		= 0,
}, {
	.match_flags    =   USB_DEVICE_ID_MATCH_INT_INFO
		 | USB_DEVICE_ID_MATCH_DEVICE,
	.idVendor               = 0x04DD,
	/* reported with some C860 units */
	.idProduct              = 0x9050,	/* C-860 */
	ZAURUS_MASTER_INTERFACE,
	.driver_info		= 0,
},

/* Olympus has some models with a Zaurus-compatible option.
 * R-1000 uses a FreeScale i.MXL cpu (ARMv4T)
 */
{
	.match_flags    =   USB_DEVICE_ID_MATCH_INT_INFO
		 | USB_DEVICE_ID_MATCH_DEVICE,
	.idVendor               = 0x07B4,
	.idProduct              = 0x0F02,	/* R-1000 */
	ZAURUS_MASTER_INTERFACE,
	.driver_info		= 0,
},

/* LG Electronics VL600 wants additional headers on every frame */
{
	USB_DEVICE_AND_INTERFACE_INFO(0x1004, 0x61aa, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* Logitech Harmony 900 - uses the pseudo-MDLM (BLAN) driver */
{
	USB_DEVICE_AND_INTERFACE_INFO(0x046d, 0xc11f, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_MDLM, USB_CDC_PROTO_NONE),
	.driver_info		= 0,
},

/* Novatel USB551L and MC551 - handled by qmi_wwan */
{
	USB_DEVICE_AND_INTERFACE_INFO(NOVATEL_VENDOR_ID, 0xB001, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* Novatel E362 - handled by qmi_wwan */
{
	USB_DEVICE_AND_INTERFACE_INFO(NOVATEL_VENDOR_ID, 0x9010, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* Dell Wireless 5800 (Novatel E362) - handled by qmi_wwan */
{
	USB_DEVICE_AND_INTERFACE_INFO(DELL_VENDOR_ID, 0x8195, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* Dell Wireless 5800 (Novatel E362) - handled by qmi_wwan */
{
	USB_DEVICE_AND_INTERFACE_INFO(DELL_VENDOR_ID, 0x8196, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* Dell Wireless 5804 (Novatel E371) - handled by qmi_wwan */
{
	USB_DEVICE_AND_INTERFACE_INFO(DELL_VENDOR_ID, 0x819b, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* Novatel Expedite E371 - handled by qmi_wwan */
{
	USB_DEVICE_AND_INTERFACE_INFO(NOVATEL_VENDOR_ID, 0x9011, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* HP lt2523 (Novatel E371) - handled by qmi_wwan */
{
	USB_DEVICE_AND_INTERFACE_INFO(HP_VENDOR_ID, 0x421d, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* AnyDATA ADU960S - handled by qmi_wwan */
{
	USB_DEVICE_AND_INTERFACE_INFO(0x16d5, 0x650a, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* Huawei E1820 - handled by qmi_wwan */
{
	USB_DEVICE_INTERFACE_NUMBER(HUAWEI_VENDOR_ID, 0x14ac, 1),
	.driver_info = 0,
},

/* Realtek RTL8153 Based USB 3.0 Ethernet Adapters */
{
	USB_DEVICE_AND_INTERFACE_INFO(REALTEK_VENDOR_ID, 0x8153, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* Lenovo Powered USB-C Travel Hub (4X90S92381, based on Realtek RTL8153) */
{
	USB_DEVICE_AND_INTERFACE_INFO(LENOVO_VENDOR_ID, 0x721e, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* Lenovo ThinkPad Hybrid USB-C with USB-A Dock (40af0135eu, based on Realtek RTL8153) */
{
	USB_DEVICE_AND_INTERFACE_INFO(LENOVO_VENDOR_ID, 0xa359, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* Aquantia AQtion USB to 5GbE Controller (based on AQC111U) */
{
	USB_DEVICE_AND_INTERFACE_INFO(AQUANTIA_VENDOR_ID, 0xc101,
				      USB_CLASS_COMM, USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* ASIX USB 3.1 Gen1 to 5G Multi-Gigabit Ethernet Adapter(based on AQC111U) */
{
	USB_DEVICE_AND_INTERFACE_INFO(ASIX_VENDOR_ID, 0x2790, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* ASIX USB 3.1 Gen1 to 2.5G Multi-Gigabit Ethernet Adapter(based on AQC112U) */
{
	USB_DEVICE_AND_INTERFACE_INFO(ASIX_VENDOR_ID, 0x2791, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* USB-C 3.1 to 5GBASE-T Ethernet Adapter (based on AQC111U) */
{
	USB_DEVICE_AND_INTERFACE_INFO(0x20f4, 0xe05a, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* QNAP QNA-UC5G1T USB to 5GbE Adapter (based on AQC111U) */
{
	USB_DEVICE_AND_INTERFACE_INFO(0x1c04, 0x0015, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = 0,
},

/* WHITELIST!!!
 *
 * CDC Ether uses two interfaces, not necessarily consecutive.
 * We match the main interface, ignoring the optional device
 * class so we could handle devices that aren't exclusively
 * CDC ether.
 *
 * NOTE:  this match must come AFTER entries blacklisting devices
 * because of bugs/quirks in a given product (like Zaurus, above).
 */
{
	/* ZTE (Vodafone) K3805-Z */
	USB_DEVICE_AND_INTERFACE_INFO(ZTE_VENDOR_ID, 0x1003, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	/* ZTE (Vodafone) K3806-Z */
	USB_DEVICE_AND_INTERFACE_INFO(ZTE_VENDOR_ID, 0x1015, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	/* ZTE (Vodafone) K4510-Z */
	USB_DEVICE_AND_INTERFACE_INFO(ZTE_VENDOR_ID, 0x1173, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	/* ZTE (Vodafone) K3770-Z */
	USB_DEVICE_AND_INTERFACE_INFO(ZTE_VENDOR_ID, 0x1177, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	/* ZTE (Vodafone) K3772-Z */
	USB_DEVICE_AND_INTERFACE_INFO(ZTE_VENDOR_ID, 0x1181, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	/* Telit modules */
	USB_VENDOR_AND_INTERFACE_INFO(0x1bc7, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = (kernel_ulong_t) &wwan_info,
}, {
	/* Dell DW5580 modules */
	USB_DEVICE_AND_INTERFACE_INFO(DELL_VENDOR_ID, 0x81ba, USB_CLASS_COMM,
			USB_CDC_SUBCLASS_ETHERNET, USB_CDC_PROTO_NONE),
	.driver_info = (kernel_ulong_t)&wwan_info,
}, {
	/* Huawei ME906 and ME909 */
	USB_DEVICE_AND_INTERFACE_INFO(HUAWEI_VENDOR_ID, 0x15c1, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	/* ZTE modules */
	USB_VENDOR_AND_INTERFACE_INFO(ZTE_VENDOR_ID, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&zte_cdc_info,
}, {
	/* U-blox TOBY-L2 */
	USB_DEVICE_AND_INTERFACE_INFO(UBLOX_VENDOR_ID, 0x1143, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	/* U-blox SARA-U2 */
	USB_DEVICE_AND_INTERFACE_INFO(UBLOX_VENDOR_ID, 0x1104, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	/* U-blox LARA-R6 01B */
	USB_DEVICE_AND_INTERFACE_INFO(UBLOX_VENDOR_ID, 0x1313, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	/* U-blox LARA-L6 */
	USB_DEVICE_AND_INTERFACE_INFO(UBLOX_VENDOR_ID, 0x1343, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	/* Cinterion PLS8 modem by GEMALTO */
	USB_DEVICE_AND_INTERFACE_INFO(0x1e2d, 0x0061, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	/* Cinterion AHS3 modem by GEMALTO */
	USB_DEVICE_AND_INTERFACE_INFO(0x1e2d, 0x0055, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	/* Cinterion PLS62-W modem by GEMALTO/THALES */
	USB_DEVICE_AND_INTERFACE_INFO(0x1e2d, 0x005b, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	/* Cinterion PLS83/PLS63 modem by GEMALTO/THALES */
	USB_DEVICE_AND_INTERFACE_INFO(0x1e2d, 0x0069, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET,
				      USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,
}, {
	USB_INTERFACE_INFO(USB_CLASS_COMM, USB_CDC_SUBCLASS_ETHERNET,
			USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long) &cdc_info,
}, {
	USB_INTERFACE_INFO(USB_CLASS_COMM, USB_CDC_SUBCLASS_MDLM,
			USB_CDC_PROTO_NONE),
	.driver_info = (unsigned long)&wwan_info,

}, {
	/* Various Huawei modems with a network port like the UMG1831 */
	USB_VENDOR_AND_INTERFACE_INFO(HUAWEI_VENDOR_ID, USB_CLASS_COMM,
				      USB_CDC_SUBCLASS_ETHERNET, 255),
	.driver_info = (unsigned long)&wwan_info,
},
	{ },		/* END */
};
MODULE_DEVICE_TABLE(usb, products);

static struct usb_driver cdc_driver = {
	.name =		"cdc_ether",
	.id_table =	products,
	.probe =	usbnet_probe,
	.disconnect =	usbnet_disconnect,
	.suspend =	usbnet_suspend,
	.resume =	usbnet_resume,
	.reset_resume =	usbnet_resume,
	.supports_autosuspend = 1,
	.disable_hub_initiated_lpm = 1,
};

module_usb_driver(cdc_driver);

MODULE_AUTHOR("David Brownell");
MODULE_DESCRIPTION("USB CDC Ethernet devices");
MODULE_LICENSE("GPL");
