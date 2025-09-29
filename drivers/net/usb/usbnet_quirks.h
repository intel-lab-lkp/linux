/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A collection of chip information to be ignored
 */

#ifndef __USB_NET_IGNORE_H__
#define __USB_NET_IGNORE_H__

#include <linux/usb.h>

/* usbnet_ignore_list:
 * Chip info which already support int vendor specific driver,
 * and then should be ignored in generic usbnet
 */
static const struct usb_device_id usbnet_ignore_list[] = {
	/* Chips already support in ax88179_178a.c */
	{ USB_DEVICE(0x0b95, 0x1790) },
	{ USB_DEVICE(0x0b95, 0x178a) },
	{ USB_DEVICE(0x04b4, 0x3610) },
	{ USB_DEVICE(0x2001, 0x4a00) },
	{ USB_DEVICE(0x0df6, 0x0072) },
	{ USB_DEVICE(0x04e8, 0xa100) },
	{ USB_DEVICE(0x17ef, 0x304b) },
	{ USB_DEVICE(0x050d, 0x0128) },
	{ USB_DEVICE(0x0930, 0x0a13) },
	{ USB_DEVICE(0x0711, 0x0179) },
	{ USB_DEVICE(0x07c9, 0x000e) },
	{ USB_DEVICE(0x07c9, 0x000f) },
	{ USB_DEVICE(0x07c9, 0x0010) },
	/* End of support in ax88179_178a.c */

	{ } /*END*/
};

static inline bool usbnet_ignore(struct usb_interface *intf)
{
	return !!usb_match_id(intf, usbnet_ignore_list);
}
#endif
