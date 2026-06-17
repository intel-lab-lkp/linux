// SPDX-License-Identifier: GPL-2.0

#include <linux/usb.h>

__rust_helper struct usb_device *
rust_helper_interface_to_usbdev(struct usb_interface *intf)
{
	return interface_to_usbdev(intf);
}

__rust_helper unsigned int
rust_helper_usb_sndbulkpipe(struct usb_device *dev, unsigned int endpoint)
{
	return usb_sndbulkpipe(dev, endpoint);
}

__rust_helper unsigned int
rust_helper_usb_rcvbulkpipe(struct usb_device *dev, unsigned int endpoint)
{
	return usb_rcvbulkpipe(dev, endpoint);
}

__rust_helper unsigned int
rust_helper_usb_rcvintpipe(struct usb_device *dev, unsigned int endpoint)
{
	return usb_rcvintpipe(dev, endpoint);
}
