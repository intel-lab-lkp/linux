// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for USB Mass Storage devices
 * Usual Tables File for usb-storage and libusual
 *
 * Copyright (C) 2009 Alan Stern (stern@rowland.harvard.edu)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb_usual.h>


/*
 * The table of devices is pre-generated in usb-ids.c
 */
#include "usb-ids.c"

/*
 * The table of devices to ignore
 */
struct ignore_entry {
	u16	vid, pid, bcdmin, bcdmax;
};

#define UNUSUAL_DEV(id_vendor, id_product, bcdDeviceMin, bcdDeviceMax, \
		    vendorName, productName, useProtocol, useTransport, \
		    initFunction, flags) \
{					\
	.vid	= id_vendor,		\
	.pid 	= id_product,		\
	.bcdmin	= bcdDeviceMin,		\
	.bcdmax = bcdDeviceMax,		\
}

static const struct ignore_entry ignore_ids[] = {
#	include "unusual_alauda.h"
#	include "unusual_cypress.h"
#	include "unusual_datafab.h"
#	include "unusual_ene_ub6250.h"
#	include "unusual_freecom.h"
#	include "unusual_isd200.h"
#	include "unusual_jumpshot.h"
#	include "unusual_karma.h"
#	include "unusual_onetouch.h"
#	include "unusual_realtek.h"
#	include "unusual_sddr09.h"
#	include "unusual_sddr55.h"
#	include "unusual_usbat.h"
	{ }		/* Terminating entry */
};

#undef UNUSUAL_DEV

/* Return an error if a device is in the ignore_ids list */
int usb_usual_ignore_device(struct usb_interface *intf)
{
	struct usb_device *udev;
	unsigned vid, pid, bcd;
	const struct ignore_entry *p;

	udev = interface_to_usbdev(intf);
	vid = le16_to_cpu(udev->descriptor.idVendor);
	pid = le16_to_cpu(udev->descriptor.idProduct);
	bcd = le16_to_cpu(udev->descriptor.bcdDevice);

	for (p = ignore_ids; p->vid; ++p) {
		if (p->vid == vid && p->pid == pid &&
				p->bcdmin <= bcd && p->bcdmax >= bcd)
			return -ENXIO;
	}
	return 0;
}
